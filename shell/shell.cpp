#include "../part/documentfeedback.h"
#include "../part/documentidentity.h"
#include "../part/readertheme.h"
#include "readerchrome.h"
#include "../part/thoughtsplatform.h"
#include "config-thoughts.h"
#include "../part/thoughtsserveraddress.h"
/*
    SPDX-FileCopyrightText: 2002 Wilco Greven <greven@kde.org>
    SPDX-FileCopyrightText: 2002 Chris Cheney <ccheney@cheney.cx>
    SPDX-FileCopyrightText: 2003 Benjamin Meyer <benjamin@csh.rit.edu>
    SPDX-FileCopyrightText: 2003-2004 Christophe Devriese <Christophe.Devriese@student.kuleuven.ac.be>
    SPDX-FileCopyrightText: 2003 Laurent Montel <montel@kde.org>
    SPDX-FileCopyrightText: 2003-2004 Albert Astals Cid <aacid@kde.org>
    SPDX-FileCopyrightText: 2003 Luboš Luňák <l.lunak@kde.org>
    SPDX-FileCopyrightText: 2003 Malcolm Hunter <malcolm.hunter@gmx.co.uk>
    SPDX-FileCopyrightText: 2004 Dominique Devriese <devriese@kde.org>
    SPDX-FileCopyrightText: 2004 Dirk Mueller <mueller@kde.org>

    Work sponsored by the LiMux project of the city of Munich:
    SPDX-FileCopyrightText: 2017 Klarälvdalens Datakonsult AB a KDAB Group company <info@kdab.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "shell.h"
#include "../core/htmlattachment.h"
#include "agentdocumentdescription.h"
#include "agentnotes.h"
#include "agentnoteswindow.h"
#include "agentproposalparser.h"
#include "../part/commentanchorstate.h"
#include "proposaldecisions.h"
#include "../part/docschangemonitor.h"
#include "readerdisplaytitlestore.h"
#include "reviewdocumenttools.h"
#include "part/verifiedartifactcache.h"
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include "developerlog.h"
#include "developerlogdialog.h"
#include "updatecheck.h"
#include "activeparttoolbarcontroller.h"
#include "automaticsyncpolicy.h"
#include "thoughtscommandprotocol.h"
#include "thoughtspagelink.h"

#include <atomic>
#include <optional>

// qt/kde includes
#include <KActionCollection>
#include <KConfigGroup>
#include <KGuiItem>
#include <KIO/Global>
#include <KIO/OpenFileManagerWindowJob>
#include <KLocalizedString>
#include <KMessageBox>
#include <KPluginFactory>
#include <KRecentFilesAction>
#include <KSharedConfig>
#include <KStandardAction>
#include <KStandardGuiItem>
#if !defined(Q_OS_WIN) && !defined(Q_OS_OSX) && !defined(Q_OS_HAIKU)
#include <KStartupInfo>
#include <KWindowInfo>
#endif
#include <KToggleFullScreenAction>
#include <KToolBar>
#include <KUrlMimeData>
#include <KWindowSystem>
#include <KXMLGUIFactory>
#include <QApplication>
#include <QPixmapCache>
#include <QElapsedTimer>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTimeEdit>
#if HAVE_DBUS
#include <QDBusConnection>
#endif // HAVE_DBUS
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDomDocument>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEasingCurve>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QUrlQuery>
#include <QFormLayout>
#include <QGraphicsOpacityEffect>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QFileSystemWatcher>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLocale>
#include <QContextMenuEvent>
#include <QMenu>
#include <QIconEngine>
#include <QMenuBar>
#include <QMessageBox>
#include <QMutex>
#include <QMutexLocker>
#include <QMimeData>
#include <QMouseEvent>
#include <QObject>
#include <QPainter>
#include <QPainterPath>
#include <QParallelAnimationGroup>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QProgressDialog>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRegularExpression>
#include <QSysInfo>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QScreen>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleOptionToolButton>
#include <QThreadPool>
#include <QStringDecoder>
#include <QTabBar>
#include <QTabWidget>
#include <QThreadPool>
#include <QTimeZone>
#include <QEventLoop>
#include <QTimer>

#include <memory>
#include <QToolButton>
#include <QScrollArea>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <QWindow>

#include <poppler-qt6.h>

#include <algorithm>
#include <optional>
#include <tuple>
#include <QScopeGuard>
#include <utility>

// local includes
#include "../interfaces/viewerinterface.h"
#include "../part/docscommentsynchronizer.h"
#include "buildterminallauncher.h"
#include "commentcodexlauncher.h"
#include "commentsoverview.h"
#include "../part/agentconversations.h"
#include "../part/libraryprojectstore.h"
#include "../part/proposalprovenance.h"
#include "../part/feedbackversion.h"
#include "../part/pdfversions.h"
#include "../part/projectconfiguration.h"
#include "documentsyncpresentation.h"
#include "kdocumentviewer.h"
#include "reviewfeedbackpanel.h"
#include "reviewoverview.h"
#include "reviewstore.h"
#include "reviewtagplan.h"
#include "todocommands.h"
#include "reviewcardeditor.h"
#include "reviewcardmerge.h"
#include "shellutils.h"
#include "syncjournal.h"
#include "syncoperationstate.h"
#include "syncstatuslabels.h"
#include "syncstatewatch.h"

static const char *shouldShowMenuBarComingFromFullScreen = "shouldShowMenuBarComingFromFullScreen";
static const char *shouldShowToolBarComingFromFullScreen = "shouldShowToolBarComingFromFullScreen";

static const char *const SESSION_URL_KEY = "Urls";
static const char *const SESSION_TAB_KEY = "ActiveTab";
// The page each tab shows ("" for a document's tab), as SESSION_URL_KEY.
static const char *const SESSION_HTML_PAGES_KEY = "HtmlAttachmentPages";
// The page path of a tab that shows one page of its document (Shell::showHtmlAttachmentTab).
static const char *const HTML_ATTACHMENT_TAB_PROPERTY = "thoughtsHtmlAttachmentTab";

static constexpr char SIDEBAR_LOCKED_KEY[] = "LockSidebar";
static constexpr char SIDEBAR_VISIBLE_KEY[] = "ShowSidebar";
static constexpr char TAB_DRAG_MIME_TYPE[] = "application/x-thoughts-tab";
static constexpr int TAB_DROP_ANIMATION_DURATION = 130;
// A refresh of the Logs window asks the journal for this many of its newest
// runs, besides the runs that reported a change.
static constexpr int SyncLogsRefreshRows = 30;
// How soon after a failed Sync the app asks for the state that Sync left there.
// How often it asks otherwise is the watch's (SyncStateWatch: 20 s, or at once
// when the server holds the question until its state changes).
static constexpr int StateAfterFailedSyncDelayMs = 1000;
static QPointer<Thoughts::SyncStateWatch> s_remoteWorkWatch;
// An ended build's Sync that could not start while the journal was rewritten is
// tried again this much later.
static constexpr int AnnouncedWorkRetryMs = 2000;
// A decision on a proposal made on this computer is sent by a Sync the app
// starts by itself this long after the last decision.
static constexpr int ProposalDecisionSyncDelayMs = 5 * 1000;
// How often the app looks whether the Sync journal can be compacted, and how
// long Thoughts must not have been the active application before it is.
static constexpr int SyncJournalUpkeepIntervalMs = 60 * 1000;
static constexpr int SyncJournalCompactIdleMs = 2 * 60 * 1000;

namespace
{
// Document tabs share the bar evenly, like a web browser's: equal widths up
// to a maximum, shrinking together as tabs are added. QTabBar's own layout
// shrinks tabs in proportion to their titles, so a few long titles kept
// wide tabs while the others were cut to a couple of letters.
class DocumentTabBar : public QTabBar
{
public:
    explicit DocumentTabBar(QWidget *parent)
        : QTabBar(parent)
    {
        setMouseTracking(true);
    }
    static constexpr int MaximumTabWidth = 240;
    static constexpr int MinimumTabWidth = 80;

protected:
    // A tab shows its close button only while the pointer is over it.
    void mouseMoveEvent(QMouseEvent *event) override
    {
        QTabBar::mouseMoveEvent(event);
        showCloseButton(tabAt(event->position().toPoint()));
    }
    void leaveEvent(QEvent *event) override
    {
        QTabBar::leaveEvent(event);
        showCloseButton(-1);
    }
    void tabLayoutChange() override
    {
        QTabBar::tabLayoutChange();
        showCloseButton(underMouse() ? tabAt(mapFromGlobal(QCursor::pos())) : -1);
    }
    QSize tabSizeHint(int index) const override
    {
        QSize hint = QTabBar::tabSizeHint(index);
        const int available = parentWidget() ? parentWidget()->width() : width();
        hint.setWidth(std::clamp(available / std::max(1, count()), MinimumTabWidth, MaximumTabWidth));
        return hint;
    }
    QSize minimumTabSizeHint(int index) const override
    {
        return tabSizeHint(index);
    }

private:
    void showCloseButton(int hovered)
    {
        for (int index = 0; index < count(); ++index) {
            if (QWidget *button = tabButton(index, QTabBar::RightSide)) {
                button->setVisible(index == hovered);
            }
        }
    }
};

class DocumentTabWidget : public QTabWidget
{
public:
    explicit DocumentTabWidget(QWidget *parent)
        : QTabWidget(parent)
    {
        setTabBar(new DocumentTabBar(this));
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QTabWidget::resizeEvent(event);
        // Tab widths follow the available width; re-lay them out.
        tabBar()->setIconSize(tabBar()->iconSize());
    }
};
}

static QString compactMenuDetail(const QString &detail)
{
    constexpr qsizetype maximumCharacters = 120;
    QString compact = detail.simplified();
    if (compact.size() > maximumCharacters) {
        compact = compact.left(maximumCharacters - 1).trimmed() + QChar(0x2026);
    }
    return compact;
}

static QString syncInitiationControlLabel(const QString &control)
{
    if (control == QLatin1String("pdf_toolbar_sync")) {
        return i18n("PDF toolbar Sync");
    }
    if (control == QLatin1String("pdf_status_menu_sync")) {
        return i18n("PDF status menu Sync");
    }
    if (control == QLatin1String("pdf_reader_sync")) {
        return i18n("PDF reader Sync");
    }
    if (control == QLatin1String("comments_overview_sync")) {
        return i18n("Overview Sync");
    }
    if (control == QLatin1String("comments_overview_source_sync")) {
        return i18n("Overview source Sync");
    }
    if (control == QLatin1String("pdf_rebuild_action")) {
        return i18n("PDF rebuild action");
    }
    if (control == QLatin1String("publish_source_document")) {
        return i18n("Publish source document");
    }
    if (control == QLatin1String("logs_retry")) {
        return i18n("Logs Retry");
    }
    if (control == QLatin1String("command_api")) {
        return i18n("Command/API request");
    }
    if (control == QLatin1String("runtime_adoption")) {
        return i18n("Recovered running sync");
    }
    if (control == QLatin1String("remote_change")) {
        return i18n("Automatic Sync after changes on the server");
    }
    if (control == QLatin1String("proposal_decision")) {
        return i18n("Automatic Sync after a proposal decision");
    }
    if (control == QLatin1String("direct_sync_request")) {
        return i18n("Direct sync request");
    }
    return i18n("Older app or unknown control");
}

// The Sync journal lives on its own thread (syncjournal.h). These keep the
// names the rest of this file uses.
static Thoughts::SyncJournal *syncJournal()
{
    return Thoughts::SyncJournal::instance();
}

QJsonArray Shell::recentSyncOperationsWithHistory()
{
    return syncJournal()->recentOperations(true);
}

QJsonArray Shell::recentSyncOperationSummaries()
{
    return syncJournal()->recentOperations(false);
}

QJsonObject Shell::syncOperationWithHistory(const QString &operationId)
{
    return syncJournal()->operationWithHistory(operationId);
}

static bool safeOperationPdfTarget(const QString &target)
{
    return !target.isEmpty() && target.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive) && !target.contains(QLatin1Char('/')) && !target.contains(QLatin1Char('\\'));
}

static QJsonObject readSyncOperationSummary(const QString &operationId)
{
    return syncJournal()->operationSummary(operationId);
}

static QList<QJsonObject> runningSyncOperationSummaries()
{
    return syncJournal()->runningSummaries();
}

QDateTime Shell::lastSuccessfulGlobalSyncAt()
{
    return syncJournal()->lastSuccessfulGlobalSyncAt();
}

QJsonObject Shell::latestGlobalSyncOperationSummary()
{
    return syncJournal()->latestGlobalSummary();
}

QJsonObject Shell::latestDocumentSyncOperationSummary(const QString &target)
{
    return safeOperationPdfTarget(target) ? syncJournal()->latestDocumentSummary(target) : QJsonObject();
}

static QString runningBuildOperationIdForTarget(const QString &target)
{
    return safeOperationPdfTarget(target) ? syncJournal()->runningBuildOperationIdForTarget(target) : QString();
}

static QString beginSyncOperation(const QString &kind, const QString &target, const QString &phase, const QString &initiationControl, const QString &retryOf = {})
{
    return syncJournal()->beginOperation(kind, target, phase, initiationControl, retryOf);
}

static void updateSyncOperation(const QString &id, Thoughts::SyncOperationState state, const QString &phase, const QString &event, const QString &buildId = {}, const QString &errorCode = {},
                                const QString &warningCode = {}, const QString &warning = {})
{
    syncJournal()->updateOperation(id, state, phase, event, buildId, errorCode, warningCode, warning);
}

static void reconcileInterruptedSyncOperations(qint64 startedBeforeMs)
{
    syncJournal()->reconcileInterrupted(startedBeforeMs);
}

namespace
{
// When Thoughts starts a Sync by itself (automaticsyncpolicy.h). One for the
// process: the server is asked once for all windows.
Thoughts::AutomaticSyncPolicy s_automaticSync;

// Who started a Sync, from the control recorded with it.
Thoughts::AutomaticSyncPolicy::Initiation automaticSyncInitiation(const QString &initiationControl)
{
    if (initiationControl == QLatin1String("remote_change") || initiationControl == QLatin1String("proposal_decision")) {
        return Thoughts::AutomaticSyncPolicy::Initiation::Automatic;
    }
    if (initiationControl == QLatin1String("command_api") || initiationControl == QLatin1String("runtime_adoption")) {
        return Thoughts::AutomaticSyncPolicy::Initiation::Command;
    }
    return Thoughts::AutomaticSyncPolicy::Initiation::User;
}

// Why a Sync failed, as the repository says with the failure itself
// ("failure_kind" of the part's fedoraManualSyncState): transient, needs the
// user, or a defect. A Sync that the shell could not even start has no kind.
Thoughts::AutomaticSyncPolicy::FailureKind automaticSyncFailureKind(const QVariantMap &manualSync)
{
    return Thoughts::AutomaticSyncPolicy::failureKindFromName(manualSync.value(QStringLiteral("failure_kind")).toString());
}

// What the status area says while automatic Syncs wait after failures.
QString automaticSyncNotice()
{
    const qint64 notBefore = s_automaticSync.notBeforeMs();
    if (notBefore <= QDateTime::currentMSecsSinceEpoch()) {
        return {};
    }
    return i18n("Next automatic Sync not before %1", QLocale().toString(QDateTime::fromMSecsSinceEpoch(notBefore).toLocalTime().time(), QLocale::ShortFormat));
}
}


class ThoughtsSyncCommandService final : public QObject
{
public:
    explicit ThoughtsSyncCommandService(QObject *parent)
        : QObject(parent)
    {
        if (startServer()) {
            const qint64 startupReconciliationCutoff = QDateTime::currentMSecsSinceEpoch();
            QTimer::singleShot(0, this, [startupReconciliationCutoff] { reconcileInterruptedSyncOperations(startupReconciliationCutoff); });
        }
    }

    ~ThoughtsSyncCommandService() override
    {
        if (m_server && m_server->isListening()) {
            m_server->close();
            QLocalServer::removeServer(Thoughts::syncCommandSocketPath());
        }
    }

    void registerShell(Shell *shell)
    {
        if (shell && !m_shells.contains(shell)) {
            m_shells.append(shell);
            if (!m_manualSyncOperationId.isEmpty() && !m_manualSyncOwner) {
                setManualSyncOwner(shell);
            }
        }
    }

    void unregisterShell(Shell *shell)
    {
        const bool transferManualSync = shell && m_manualSyncOwner == shell && !m_manualSyncOperationId.isEmpty();
        m_shells.removeAll(shell);
        removeDeletedShells();
        if (transferManualSync) {
            m_manualSyncOwner.clear();
            if (Shell *replacement = commandShell()) {
                setManualSyncOwner(replacement);
            }
        }
    }

    bool hasActiveManualSync() const
    {
        return !m_manualSyncOperationId.isEmpty();
    }

    QString activeManualSyncOperationId() const
    {
        return m_manualSyncOperationId;
    }

    bool claimManualSync(Shell *shell, const QString &operationId)
    {
        if (!shell || operationId.isEmpty() || (!m_manualSyncOperationId.isEmpty() && m_manualSyncOperationId != operationId) || (m_manualSyncOwner && m_manualSyncOwner != shell)) {
            return false;
        }
        m_manualSyncOperationId = operationId;
        setManualSyncOwner(shell);
        m_lastManualSyncForward.clear();
        return true;
    }

    bool prepareManualSyncState(Shell *shell, int state, const QString &target, const QString &message, const QString &details, QString *operationId)
    {
        if (!shell || !operationId || state < 1 || state > 3) {
            return false;
        }
        if (m_manualSyncOperationId.isEmpty() && state == 1) {
            const QString adoptedOperationId = beginSyncOperation(QStringLiteral("sync"), target, QStringLiteral("queued"), QStringLiteral("runtime_adoption"));
            if (!claimManualSync(shell, adoptedOperationId)) {
                return false;
            }
        }
        if (m_manualSyncOperationId.isEmpty()) {
            return false;
        }
        if (m_manualSyncOwner && m_manualSyncOwner != shell) {
            if (!m_manualSyncOwner->m_tabs.isEmpty()) {
                return false;
            }
            setManualSyncOwner(shell);
        }
        if (!m_manualSyncOwner) {
            setManualSyncOwner(shell);
        }

        const QString forwardIdentity = QString::number(state) + QChar(0x1f) + message + QChar(0x1f) + details;
        if (m_lastManualSyncForward == forwardIdentity) {
            return false;
        }
        m_lastManualSyncForward = forwardIdentity;
        QTimer::singleShot(0, this, [this, forwardIdentity] {
            if (m_lastManualSyncForward == forwardIdentity) {
                m_lastManualSyncForward.clear();
            }
        });
        *operationId = m_manualSyncOperationId;
        return true;
    }

    void completeManualSync(Shell *shell, const QString &operationId)
    {
        if (!shell || shell != m_manualSyncOwner || operationId.isEmpty() || operationId != m_manualSyncOperationId) {
            return;
        }
        if (shell->m_activeManualSyncOperationId == operationId) {
            shell->m_activeManualSyncOperationId.clear();
        }
        m_manualSyncOwner.clear();
        m_manualSyncOperationId.clear();
        m_lastManualSyncForward.clear();
    }

private:
    bool startServer()
    {
        const QString socketPath = Thoughts::syncCommandSocketPath();
#ifdef Q_OS_WIN
        // Named pipes allow several servers with one name, so listen() alone
        // would not notice another running Thoughts.
        {
            QLocalSocket probe;
            probe.connectToServer(socketPath);
            if (probe.waitForConnected(150)) {
                qWarning() << "Another Thoughts already serves" << socketPath;
                return false;
            }
        }
#else
        if (!QDir().mkpath(QFileInfo(socketPath).absolutePath())) {
            qWarning() << "Could not create the Thoughts command-service directory";
            return false;
        }
#endif

        auto *server = new QLocalServer(this);
        server->setSocketOptions(QLocalServer::UserAccessOption);
        if (!server->listen(socketPath)) {
            QLocalSocket probe;
            probe.connectToServer(socketPath);
            if (probe.waitForConnected(150)) {
                delete server;
                return false;
            }
            if (probe.error() != QLocalSocket::ServerNotFoundError && probe.error() != QLocalSocket::ConnectionRefusedError) {
                qWarning() << "Could not verify that the existing Thoughts command-service socket is stale:" << probe.errorString();
                delete server;
                return false;
            }
            QLocalServer::removeServer(socketPath);
            if (!server->listen(socketPath)) {
                qWarning() << "Could not start the Thoughts command service:" << server->errorString();
                delete server;
                return false;
            }
        }
        m_server = server;
        connect(server, &QLocalServer::newConnection, this, [this] { acceptConnections(); });
        return true;
    }

    void removeDeletedShells()
    {
        m_shells.removeIf([](const QPointer<Shell> &shell) { return shell.isNull(); });
    }

    Shell *commandShell()
    {
        removeDeletedShells();
        if (auto *activeShell = qobject_cast<Shell *>(QApplication::activeWindow()); activeShell && m_shells.contains(activeShell)) {
            return activeShell;
        }
        for (const QPointer<Shell> &shell : std::as_const(m_shells)) {
            if (shell && shell->isVisible()) {
                return shell;
            }
        }
        return m_shells.isEmpty() ? nullptr : m_shells.constFirst().data();
    }

    void setManualSyncOwner(Shell *shell)
    {
        if (m_manualSyncOwner && m_manualSyncOwner != shell && m_manualSyncOwner->m_activeManualSyncOperationId == m_manualSyncOperationId) {
            m_manualSyncOwner->m_activeManualSyncOperationId.clear();
        }
        m_manualSyncOwner = shell;
        if (shell) {
            shell->m_activeManualSyncOperationId = m_manualSyncOperationId;
        }
    }

    void acceptConnections()
    {
        while (m_server && m_server->hasPendingConnections()) {
            QLocalSocket *socket = m_server->nextPendingConnection();
            if (!socket) {
                continue;
            }
            socket->setParent(this);
            connect(socket, &QLocalSocket::readyRead, this, [this, socket] { processSocket(socket); });
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            if (socket->bytesAvailable() > 0) {
                processSocket(socket);
            }
        }
    }

    void processSocket(QLocalSocket *socket)
    {
        if (!socket || socket->property("thoughts_command_finished").toBool()) {
            return;
        }
        QByteArray input = socket->property("thoughts_command_input").toByteArray();
        input += socket->readAll();
        if (input.size() > 64 * 1024) {
            finishSocket(socket, Thoughts::syncCommandError(QStringLiteral("invalid_request"), QStringLiteral("Command request is too large.")));
            return;
        }
        const qsizetype newline = input.indexOf('\n');
        if (newline < 0) {
            socket->setProperty("thoughts_command_input", input);
            return;
        }

        const QJsonDocument document = QJsonDocument::fromJson(input.left(newline));
        QJsonObject response;
        if (!document.isObject()) {
            response = Thoughts::syncCommandError(QStringLiteral("invalid_request"), QStringLiteral("Command request must be a JSON object."));
        } else if (Shell *shell = commandShell()) {
            response = shell->handleSyncCommandRequest(document.object());
        } else {
            response = Thoughts::syncCommandError(QStringLiteral("app_unavailable"), QStringLiteral("No Thoughts window is available for synchronization."));
        }
        finishSocket(socket, response);
    }

    static void finishSocket(QLocalSocket *socket, const QJsonObject &response)
    {
        socket->setProperty("thoughts_command_finished", true);
        socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
        socket->flush();
        socket->disconnectFromServer();
    }

    QLocalServer *m_server = nullptr;
    QList<QPointer<Shell>> m_shells;
    QPointer<Shell> m_manualSyncOwner;
    QString m_manualSyncOperationId;
    QString m_lastManualSyncForward;
};

static QPointer<ThoughtsSyncCommandService> s_syncCommandService;

static ThoughtsSyncCommandService *syncCommandService(bool create)
{
    if (!s_syncCommandService && create && QCoreApplication::instance()) {
        s_syncCommandService = new ThoughtsSyncCommandService(QCoreApplication::instance());
    }
    return s_syncCommandService;
}
static bool repeatedSyncSignal(const QString &traceId, const QJsonObject &event);

static QString fileSha256(const QString &path);

struct LocalLibraryEntry {
    bool available = false;
    bool found = false;
    bool generationCurrent = false;
    QString kind;
    QString sha256;
    QString displayName;
    QDateTime publishedAt;
    bool pdfStale = false;
    QString latestBuildStatus;
    QString latestBuildMessage;
    QString project;
    QString inputSnapshotSha256;
    QString publicationBuildId;
    QString latestBuildId;
    QString lastCompletedBuildId;
    bool localFreshnessAvailable = false;
    bool localSourcesCurrent = false;
    QString localFreshnessMessage;
    QStringList localFreshnessWatchFiles;
    QStringList localFreshnessWatchDirectories;
};

struct LocalSourceFreshness {
    bool available = false;
    bool current = false;
    QString message;
    QStringList watchFiles;
    QStringList watchDirectories;
};

static LocalSourceFreshness evaluateLocalSourceFreshness(const QString &docsRoot,
                                                         const QString &project,
                                                         const QString &sourceManifestPath,
                                                         const QString &publicationStatePath,
                                                         const QString &target,
                                                         const QString &expectedInputSnapshotSha256,
                                                         const std::optional<QJsonObject> &providedBuildInputInspection = std::nullopt);

static bool isCurrentLibraryPdf(const QString &docsRoot, const QFileInfo &pdf)
{
    const QFileInfo currentPdfs(QDir(docsRoot).filePath(QStringLiteral("current_pdfs")));
    if (QDir::cleanPath(pdf.absolutePath()) == QDir::cleanPath(currentPdfs.absoluteFilePath())) {
        return true;
    }
    const QString canonicalCurrentPdfs = currentPdfs.canonicalFilePath();
    return !canonicalCurrentPdfs.isEmpty() && pdf.canonicalPath() == canonicalCurrentPdfs;
}

enum class MainToolBarItemType {
    Action,
    Separator,
    Spacer,
};

struct MainToolBarItem {
    MainToolBarItemType type;
    QString name;
};

class ScopedWidgetUpdatesBlocker
{
public:
    explicit ScopedWidgetUpdatesBlocker(QWidget *widget)
        : m_widget(widget)
        , m_restoreUpdates(widget && widget->updatesEnabled())
    {
        if (m_restoreUpdates) {
            widget->setUpdatesEnabled(false);
        }
    }

    ~ScopedWidgetUpdatesBlocker()
    {
        if (m_widget && m_restoreUpdates) {
            m_widget->setUpdatesEnabled(true);
            m_widget->update();
        }
    }

private:
    QPointer<QWidget> m_widget;
    bool m_restoreUpdates = false;
};

static std::optional<QList<MainToolBarItem>> configuredMainToolBarItems(const QDomDocument &document)
{
    const QDomNodeList toolBars = document.elementsByTagName(QStringLiteral("ToolBar"));
    for (int i = 0; i < toolBars.size(); ++i) {
        const QDomElement toolBar = toolBars.at(i).toElement();
        if (toolBar.attribute(QStringLiteral("name")) != QLatin1StringView("mainToolBar")) {
            continue;
        }

        QList<MainToolBarItem> items;
        int unnamedSpacer = 0;
        for (QDomNode node = toolBar.firstChild(); !node.isNull(); node = node.nextSibling()) {
            const QDomElement element = node.toElement();
            if (element.isNull()) {
                continue;
            }
            const QString tagName = element.tagName();
            if (tagName.compare(QLatin1StringView("Action"), Qt::CaseInsensitive) == 0) {
                const QString name = element.attribute(QStringLiteral("name"));
                if (!name.isEmpty()) {
                    items.append({MainToolBarItemType::Action, name});
                }
            } else if (tagName.compare(QLatin1StringView("Separator"), Qt::CaseInsensitive) == 0) {
                items.append({MainToolBarItemType::Separator, element.attribute(QStringLiteral("name"))});
            } else if (tagName.compare(QLatin1StringView("Spacer"), Qt::CaseInsensitive) == 0) {
                QString name = element.attribute(QStringLiteral("name"));
                if (name.isEmpty()) {
                    name = QStringLiteral("okular_spacer_%1").arg(unnamedSpacer++);
                }
                items.append({MainToolBarItemType::Spacer, name});
            }
        }
        return items;
    }
    return std::nullopt;
}

static LocalLibraryEntry localLibraryEntry(const QString &docsRoot, const QString &filename)
{
    // The catalogue can declare a source document before its PDF reaches this client.
    LocalLibraryEntry declaredEntry;
    QFile catalogueFile(QDir(docsRoot).filePath(QStringLiteral(".thoughts/library-projects.json")));
    const QJsonObject catalogue = catalogueFile.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(catalogueFile.readAll()).object() : QJsonObject();
    for (const QJsonValue &project : catalogue.value(QStringLiteral("projects")).toArray()) {
        for (const QJsonValue &value : project.toObject().value(QStringLiteral("documents")).toArray()) {
            const QJsonObject declared = value.toObject();
            if (declared.value(QStringLiteral("filename")).toString() == filename
                && !declared.value(QStringLiteral("source_project")).toString().isEmpty()) {
                LocalLibraryEntry result;
                result.available = result.found = true;
                result.kind = QStringLiteral("source_linked");
                result.project = declared.value(QStringLiteral("source_project")).toString();
                result.displayName = declared.value(QStringLiteral("name")).toString();
                result.pdfStale = true;
                declaredEntry = result;
            }
        }
    }
    QFile index(QDir(docsRoot).filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json")));
    if (!index.open(QIODevice::ReadOnly)) {
        return declaredEntry;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(index.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject() || document.object().value(QStringLiteral("schema_version")).toInt() != 1) {
        return declaredEntry;
    }
    const QJsonValue pdfsValue = document.object().value(QStringLiteral("pdfs"));
    if (!pdfsValue.isArray()) {
        return declaredEntry;
    }
    static const QRegularExpression filenamePattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]*\\.pdf$"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression sha256Pattern(QStringLiteral("^[0-9a-f]{64}$"));
    QSet<QString> filenames;
    LocalLibraryEntry selected;
    selected.available = true;
    for (const QJsonValue &value : pdfsValue.toArray()) {
        if (!value.isObject()) {
            return declaredEntry;
        }
        const QJsonObject entry = value.toObject();
        const QString entryFilename = entry.value(QStringLiteral("filename")).toString();
        const QString kind = entry.value(QStringLiteral("kind")).toString();
        const QString sha256 = entry.value(QStringLiteral("pdf_sha256")).toString();
        if (!filenamePattern.match(entryFilename).hasMatch() || filenames.contains(entryFilename) || (kind != QLatin1String("source_linked") && kind != QLatin1String("unlinked")) || !sha256Pattern.match(sha256).hasMatch()) {
            return declaredEntry;
        }
        filenames.insert(entryFilename);
        const QString project = entry.value(QStringLiteral("project")).toString().trimmed();
        if (entryFilename == filename) {
            selected.found = true;
            selected.kind = kind;
            selected.sha256 = sha256;
            selected.displayName = entry.value(QStringLiteral("display_name")).toString().trimmed();
            selected.pdfStale = entry.value(QStringLiteral("pdf_stale")).toBool();
            selected.project = project;
            selected.inputSnapshotSha256 = entry.value(QStringLiteral("input_snapshot_sha256")).toString().trimmed();
            selected.publicationBuildId = entry.value(QStringLiteral("build_id")).toString().trimmed();
            const QJsonObject latestBuild = entry.value(QStringLiteral("latest_build")).toObject();
            selected.latestBuildId = latestBuild.value(QStringLiteral("id")).toString().trimmed();
            selected.latestBuildStatus = latestBuild.value(QStringLiteral("status")).toString().trimmed();
            selected.latestBuildMessage = latestBuild.value(QStringLiteral("error")).toString().trimmed();
            const QJsonArray failedDocuments = latestBuild.value(QStringLiteral("failed_documents")).toArray();
            if (selected.latestBuildMessage.isEmpty() && !failedDocuments.isEmpty()) {
                selected.latestBuildMessage = failedDocuments.first().toObject().value(QStringLiteral("message")).toString().trimmed();
            }
            selected.publishedAt = QDateTime::fromString(entry.value(QStringLiteral("published_at")).toString(), Qt::ISODateWithMs);
            if (!selected.publishedAt.isValid()) {
                selected.publishedAt = QDateTime::fromString(entry.value(QStringLiteral("published_at")).toString(), Qt::ISODate);
            }
            selected.publishedAt = selected.publishedAt.toUTC();
        }
    }
    if (selected.found) {
        QFile sourceIndexFile(QDir(docsRoot).filePath(QStringLiteral("current_pdfs/.thoughts-source-index.json")));
        const QJsonObject sourceIndex = sourceIndexFile.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(sourceIndexFile.readAll()).object() : QJsonObject();
        for (const QJsonValue &value : sourceIndex.value(QStringLiteral("pdfs")).toArray()) {
            const QJsonObject sourcePdf = value.toObject();
            const QString project = sourcePdf.value(QStringLiteral("project")).toString().trimmed();
            if (sourcePdf.value(QStringLiteral("filename")).toString() == filename && selected.project.isEmpty()) {
                selected.project = project;
            }
        }
        const bool latestBuildTerminal = selected.latestBuildStatus == QLatin1String("succeeded") || selected.latestBuildStatus == QLatin1String("attention") || selected.latestBuildStatus == QLatin1String("failed") ||
            selected.latestBuildStatus == QLatin1String("cancelled");
        selected.lastCompletedBuildId = latestBuildTerminal && !selected.latestBuildId.isEmpty() ? selected.latestBuildId : selected.publicationBuildId;

        const QString revision = document.object().value(QStringLiteral("library_revision")).toString();
        const QString sourceIndexSha256 = document.object().value(QStringLiteral("source_index_sha256")).toString();
        QFile manifestFile(QDir(docsRoot).filePath(QStringLiteral("current_pdfs/.thoughts-generation.json")));
        const QJsonObject manifest = manifestFile.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(manifestFile.readAll()).object() : QJsonObject();
        bool publicationMatches = false;
        for (const QJsonValue &value : manifest.value(QStringLiteral("publications")).toArray()) {
            const QJsonObject publication = value.toObject();
            if (publication.value(QStringLiteral("filename")).toString() == filename && publication.value(QStringLiteral("sha256")).toString() == selected.sha256) {
                publicationMatches = true;
                break;
            }
        }
        selected.generationCurrent = !revision.isEmpty() && !sourceIndexSha256.isEmpty() && manifest.value(QStringLiteral("library_revision")).toString() == revision &&
            manifest.value(QStringLiteral("source_index_sha256")).toString() == sourceIndexSha256 && publicationMatches;

        if (selected.kind == QLatin1String("source_linked") && !selected.project.isEmpty()) {
            const QString sourceManifestPath = QDir(Thoughts::Platform::thoughtsDataRoot()).filePath(QStringLiteral("source-manifest.json"));
            const QString publicationStatePath = QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server/source-publication.json"));
            const LocalSourceFreshness freshness = evaluateLocalSourceFreshness(docsRoot, selected.project, sourceManifestPath, publicationStatePath, QString(), QString());
            selected.localFreshnessAvailable = freshness.available;
            selected.localSourcesCurrent = freshness.current;
            selected.localFreshnessMessage = freshness.message;
            selected.localFreshnessWatchFiles = freshness.watchFiles;
            selected.localFreshnessWatchDirectories = freshness.watchDirectories;
        }
    }
    return selected.found ? selected : (declaredEntry.found ? declaredEntry : selected);
}

Shell::TargetSyncMode Shell::targetSyncMode(bool libraryAvailable, bool targetFound, const QString &kind)
{
    if (!libraryAvailable || !targetFound) {
        return TargetSyncMode::Reconcile;
    }
    if (kind == QLatin1String("source_linked")) {
        return TargetSyncMode::PublishSources;
    }
    if (kind == QLatin1String("unlinked")) {
        return TargetSyncMode::SnapshotOnly;
    }
    return TargetSyncMode::Reconcile;
}

QString Shell::normalizedRetrySyncTarget(const QString &storedTarget)
{
    if (storedTarget.isEmpty() || storedTarget == i18n("All documents") || storedTarget == QLatin1String("All documents")) {
        return {};
    }
    return storedTarget;
}

bool Shell::dispatchTargetSync(QObject *part, const QString &target, TargetSyncMode mode, const QString &traceId)
{
    return part && safeOperationPdfTarget(target) && mode != TargetSyncMode::Reconcile &&
        QMetaObject::invokeMethod(part, "updateDocumentFromFedora", Qt::QueuedConnection, Q_ARG(QString, target), Q_ARG(bool, mode == TargetSyncMode::PublishSources), Q_ARG(QString, traceId));
}

static QString publicationStatus(const LocalLibraryEntry &entry)
{
    if (!entry.generationCurrent) {
        return i18n("Library publication identity is not verified.");
    }
    if (!entry.publishedAt.isValid()) {
        return entry.pdfStale ? i18n("Last-good Library publication · latest server build failed: %1", entry.latestBuildMessage.isEmpty() ? i18n("unknown error") : entry.latestBuildMessage) : i18n("Current Library publication.");
    }
    if (entry.pdfStale) {
        return i18n("Last-good Library publication · published %1 · latest server build failed: %2",
                    QLocale().toString(entry.publishedAt.toLocalTime(), QLocale::ShortFormat),
                    entry.latestBuildMessage.isEmpty() ? i18n("unknown error") : entry.latestBuildMessage);
    }
    return i18n("Current Library publication · published %1.", QLocale().toString(entry.publishedAt.toLocalTime(), QLocale::ShortFormat));
}

static QString fileSha256(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}

static LocalSourceFreshness evaluateLocalSourceFreshness(const QString &docsRoot,
                                                         const QString &project,
                                                         const QString &sourceManifestPath,
                                                         const QString &publicationStatePath,
                                                         const QString &target,
                                                         const QString &expectedInputSnapshotSha256,
                                                         const std::optional<QJsonObject> &providedBuildInputInspection)
{
    LocalSourceFreshness result;
    QSet<QString> watchFiles;
    QSet<QString> watchDirectories;
    const auto addWatchPath = [&watchFiles, &watchDirectories](const QString &path) {
        const QFileInfo info(path);
        if (info.isFile()) {
            watchFiles.insert(info.absoluteFilePath());
        } else {
            // Watch the parent only while the exact file is absent. Watching
            // every parent makes unrelated siblings (for example the Fedora
            // progress cache) look like source changes.
            watchDirectories.insert(info.absolutePath());
        }
    };
    const auto finishWatchPaths = [&result, &watchFiles, &watchDirectories] {
        result.watchFiles = watchFiles.values();
        result.watchDirectories = watchDirectories.values();
        std::sort(result.watchFiles.begin(), result.watchFiles.end());
        std::sort(result.watchDirectories.begin(), result.watchDirectories.end());
    };
    addWatchPath(sourceManifestPath);
    addWatchPath(publicationStatePath);

    QFile sourceManifestFile(sourceManifestPath);
    QFile publicationStateFile(publicationStatePath);
    QJsonParseError manifestError;
    QJsonParseError stateError;
    const QJsonDocument manifestDocument = sourceManifestFile.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(sourceManifestFile.readAll(), &manifestError) : QJsonDocument();
    const QJsonDocument stateDocument = publicationStateFile.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(publicationStateFile.readAll(), &stateError) : QJsonDocument();
    const QJsonObject manifest = manifestDocument.object();
    const QJsonObject state = stateDocument.object();
    const int manifestSchema = manifest.value(QStringLiteral("schema_version")).toInt();
    QJsonArray files;
    int matchedProjects = 0;
    if (manifestSchema == 3 && manifest.value(QStringLiteral("projects")).isArray()) {
        for (const QJsonValue &projectValue : manifest.value(QStringLiteral("projects")).toArray()) {
            const QJsonObject projectObject = projectValue.toObject();
            if (projectObject.value(QStringLiteral("project")).toString() == project) {
                ++matchedProjects;
                files = projectObject.value(QStringLiteral("files")).toArray();
            }
        }
    }
    const QJsonObject bases = state.value(QStringLiteral("bases")).toObject();
    if (manifestError.error != QJsonParseError::NoError || stateError.error != QJsonParseError::NoError || manifestSchema != 3 || matchedProjects != 1 ||
        (manifest.contains(QStringLiteral("shared_files")) && !manifest.value(QStringLiteral("shared_files")).isArray()) || !manifest.value(QStringLiteral("shared_files")).toArray().isEmpty() || state.value(QStringLiteral("schema_version")).toInt() != 1 || files.isEmpty() || bases.isEmpty()) {
        result.message = i18n("The local source publication state is unavailable.");
        finishWatchPaths();
        return result;
    }

    static const QRegularExpression sha256Pattern(QStringLiteral("^[0-9a-f]{64}$"));
    const bool verifyBuildInputs = !target.isEmpty() || !expectedInputSnapshotSha256.isEmpty();
    QJsonObject buildInputInspection;
    if (verifyBuildInputs) {
        if (target.isEmpty() || !sha256Pattern.match(expectedInputSnapshotSha256).hasMatch()) {
            result.message = i18n("The latest server publication has no verifiable build-input identity.");
            finishWatchPaths();
            return result;
        }
        if (!providedBuildInputInspection.has_value()) {
            result.message = i18n("Exact build-input verification is available only through Build & Sync.");
            finishWatchPaths();
            return result;
        }
        buildInputInspection = *providedBuildInputInspection;
        const int inspectionSchema = buildInputInspection.value(QStringLiteral("schema_version")).toInt();
        if (inspectionSchema != 3 || buildInputInspection.value(QStringLiteral("target")).toString() != target ||
            !sha256Pattern.match(buildInputInspection.value(QStringLiteral("sha256")).toString()).hasMatch() || !buildInputInspection.value(QStringLiteral("entries")).isArray() ||
            !buildInputInspection.value(QStringLiteral("managed_files")).isArray() || !buildInputInspection.value(QStringLiteral("watch_files")).isArray() || !buildInputInspection.value(QStringLiteral("watch_directories")).isArray() ||
            buildInputInspection.value(QStringLiteral("project")).toString() != project || buildInputInspection.value(QStringLiteral("discovery_policy")).toString() != QLatin1String("tex-closure-v1")) {
            result.message = i18n("The local build-input inspector returned invalid provenance.");
            finishWatchPaths();
            return result;
        }
        for (const QJsonValue &value : buildInputInspection.value(QStringLiteral("watch_files")).toArray()) {
            if (!value.isString() || !QDir::isAbsolutePath(value.toString())) {
                result.message = i18n("The local build-input inspector returned invalid watch paths.");
                finishWatchPaths();
                return result;
            }
            addWatchPath(value.toString());
        }
        for (const QJsonValue &value : buildInputInspection.value(QStringLiteral("watch_directories")).toArray()) {
            if (!value.isString() || !QDir::isAbsolutePath(value.toString())) {
                result.message = i18n("The local build-input inspector returned invalid watch paths.");
                finishWatchPaths();
                return result;
            }
            watchDirectories.insert(QDir::cleanPath(value.toString()));
        }
    }

    const auto relevant = [&project](const QString &path) {
        const QString clean = QDir::cleanPath(path);
        return !QDir::isAbsolutePath(clean) && Thoughts::SourceOwner::of(clean) == project;
    };

    QSet<QString> managedPaths;
    for (const QJsonValue &value : files) {
        const QString rawPath = value.toObject().value(QStringLiteral("path")).toString();
        const QString role = value.toObject().value(QStringLiteral("role")).toString();
        if (!value.isObject() || rawPath.isEmpty() || role.isEmpty()) {
            result.message = i18n("The local source publication state is unavailable.");
            finishWatchPaths();
            return result;
        }
        const QString path = QDir::cleanPath(rawPath);
        if (relevant(path)) {
            managedPaths.insert(path);
        }
    }
    if (verifyBuildInputs) {
        for (const QJsonValue &value : buildInputInspection.value(QStringLiteral("managed_files")).toArray()) {
            if (!value.isString()) {
                result.message = i18n("The local build-input inspector returned invalid managed-source provenance.");
                finishWatchPaths();
                return result;
            }
            const QString path = QDir::cleanPath(value.toString());
            if (relevant(path)) {
                managedPaths.insert(path);
            }
        }
    }
    QSet<QString> publishedPaths;
    for (auto iterator = bases.begin(); iterator != bases.end(); ++iterator) {
        const QString path = QDir::cleanPath(iterator.key());
        if (relevant(path)) {
            publishedPaths.insert(path);
        }
    }
    QSet<QString> relevantPaths = managedPaths;
    relevantPaths.unite(publishedPaths);
    if (relevantPaths.isEmpty()) {
        result.message = i18n("The local source publication state is unavailable.");
        finishWatchPaths();
        return result;
    }

    QStringList orderedPaths = relevantPaths.values();
    std::sort(orderedPaths.begin(), orderedPaths.end());
    result.available = true;
    result.current = true;
    for (const QString &relativePath : std::as_const(orderedPaths)) {
        const QString absolutePath = QDir(docsRoot).filePath(relativePath);
        addWatchPath(absolutePath);
        const QFileInfo sourceInfo(absolutePath);
        const QString publishedSha256 = bases.value(relativePath).toObject().value(QStringLiteral("sha256")).toString();
        if (result.current && (!managedPaths.contains(relativePath) || !sourceInfo.isFile() || !sha256Pattern.match(publishedSha256).hasMatch() || fileSha256(absolutePath) != publishedSha256)) {
            result.current = false;
            result.message = i18n("Local source changed after the last server publication: %1", relativePath);
        }
    }
    if (result.current) {
        if (verifyBuildInputs && buildInputInspection.value(QStringLiteral("sha256")).toString() != expectedInputSnapshotSha256) {
            result.current = false;
            result.message = i18n("Local build inputs changed after the last server publication.");
        } else if (verifyBuildInputs) {
            result.message = i18n("Local sources and build inputs match the latest server publication.");
        } else {
            result.message = i18n("Local source hashes match the latest server publication.");
        }
    }
    finishWatchPaths();
    return result;
}

static bool readablePdfPayload(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && file.read(5) == QByteArrayLiteral("%PDF-");
}

static void migrateLegacyMainToolbarConfig()
{
    const QString configRoot = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).filePath(QStringLiteral("kxmlgui5/okular"));
    const QString partPath = QDir(configRoot).filePath(QStringLiteral("part.rc"));
    const QString shellPath = QDir(configRoot).filePath(QStringLiteral("shell.rc"));
    QFile partFile(partPath);
    QFile shellFile(shellPath);
    if (!partFile.open(QIODevice::ReadOnly) || !shellFile.open(QIODevice::ReadOnly)) {
        return;
    }
    QString partXml = QString::fromUtf8(partFile.readAll());
    QString shellXml = QString::fromUtf8(shellFile.readAll());
    // Windows cannot replace files that are still open.
    partFile.close();
    shellFile.close();
    const QRegularExpression toolbarExpression(QStringLiteral("<ToolBar\\b(?=[^>]*\\bname=[\"']mainToolBar[\"'])[^>]*>(.*?)</ToolBar>"), QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch legacyMatch = toolbarExpression.match(partXml);
    const QRegularExpressionMatch shellMatch = toolbarExpression.match(shellXml);
    if (!legacyMatch.hasMatch() || !shellMatch.hasMatch()) {
        return;
    }

    const QString backupPath = partPath + QStringLiteral(".pre-single-toolbar");
    if (!QFileInfo::exists(backupPath)) {
        QFile::copy(partPath, backupPath);
    }
    const QString shellBackupPath = shellPath + QStringLiteral(".pre-single-toolbar");
    if (!QFileInfo::exists(shellBackupPath)) {
        QFile::copy(shellPath, shellBackupPath);
    }

    const QString migratedToolbar = QStringLiteral("<ToolBar name=\"mainToolBar\" noMerge=\"1\">") + legacyMatch.captured(1) + QStringLiteral("</ToolBar>");
    shellXml.replace(shellMatch.capturedStart(), shellMatch.capturedLength(), migratedToolbar);
    // Keep migrated legacy content below the bundled shell.rc version. This
    // lets KXMLGUI replace it with the maintained toolbar instead of treating
    // stale user content as the current layout.
    shellXml.replace(QRegularExpression(QStringLiteral("version=[\"']\\d+[\"']")), QStringLiteral("version=\"20\""));
    partXml.remove(legacyMatch.capturedStart(), legacyMatch.capturedLength());

    QSaveFile savedPart(partPath);
    QSaveFile savedShell(shellPath);
    savedPart.setDirectWriteFallback(false);
    savedShell.setDirectWriteFallback(false);
    if (!savedPart.open(QIODevice::WriteOnly) || !savedShell.open(QIODevice::WriteOnly)) {
        return;
    }
    if (savedPart.write(partXml.toUtf8()) != partXml.toUtf8().size() || savedShell.write(shellXml.toUtf8()) != shellXml.toUtf8().size() || !savedShell.commit()) {
        savedPart.cancelWriting();
        savedShell.cancelWriting();
        return;
    }
    if (!savedPart.commit()) {
        savedPart.cancelWriting();
    }
}

static bool repairShellMenuLabelsDocument(QDomDocument &document)
{
    const QDomElement menuBar = document.documentElement().firstChildElement(QStringLiteral("MenuBar"));
    if (menuBar.isNull()) {
        return false;
    }

    const QHash<QString, QString> labels {
        {QStringLiteral("file"), QStringLiteral("&File")},
        {QStringLiteral("settings"), QStringLiteral("&Settings")},
        {QStringLiteral("help"), QStringLiteral("&Help")},
    };
    bool changed = false;
    for (QDomElement menu = menuBar.firstChildElement(QStringLiteral("Menu")); !menu.isNull(); menu = menu.nextSiblingElement(QStringLiteral("Menu"))) {
        const auto expected = labels.constFind(menu.attribute(QStringLiteral("name")));
        if (expected == labels.cend()) {
            continue;
        }
        QDomElement textElement = menu.firstChildElement(QStringLiteral("text"));
        if (textElement.isNull()) {
            textElement = document.createElement(QStringLiteral("text"));
            textElement.appendChild(document.createTextNode(*expected));
            menu.insertBefore(textElement, menu.firstChild());
            changed = true;
        } else if (textElement.text() != *expected) {
            while (!textElement.firstChild().isNull()) {
                textElement.removeChild(textElement.firstChild());
            }
            textElement.appendChild(document.createTextNode(*expected));
            changed = true;
        }
    }
    return changed;
}

bool Shell::repairShellMenuLabels(QDomDocument &document)
{
    return repairShellMenuLabelsDocument(document);
}

// Draw the same overlapping-card outline at every display scale and in both themes.
class ReviewToolbarIconEngine final : public QIconEngine
{
public:
    QIconEngine *clone() const override { return new ReviewToolbarIconEngine; }
    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State) override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const int side = qMin(rect.width(), rect.height());
        painter->translate(rect.center().x() - side / 2.0, rect.center().y() - side / 2.0);
        painter->scale(side / 24.0, side / 24.0);
        const QColor color = qApp->palette().color(mode == QIcon::Disabled ? QPalette::Disabled : QPalette::Active, QPalette::WindowText);
        painter->setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->setBrush(Qt::NoBrush);
        QPainterPath back;
        back.moveTo(8, 6);
        back.lineTo(8, 4);
        back.quadTo(8, 2, 10, 2);
        back.lineTo(20, 2);
        back.quadTo(22, 2, 22, 4);
        back.lineTo(22, 16);
        back.quadTo(22, 18, 20, 18);
        back.lineTo(18, 18);
        painter->drawPath(back);
        painter->drawRoundedRect(QRectF(2, 6, 16, 16), 2, 2);
        painter->restore();
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        QPixmap result(size);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        paint(&painter, QRect(QPoint(), size), mode, state);
        return result;
    }
};

// A speech bubble; a red dot at its corner while agent notes are unread.
class AgentNotesToolbarIconEngine final : public QIconEngine
{
public:
    explicit AgentNotesToolbarIconEngine(bool unread)
        : m_unread(unread)
    {
    }
    QIconEngine *clone() const override { return new AgentNotesToolbarIconEngine(m_unread); }
    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State) override
    {
        // Drawn into an image first: clearing room for the dot must not cut through the toolbar.
        const qreal ratio = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
        QImage image((QSizeF(rect.size()) * ratio).toSize(), QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(ratio);
        image.fill(Qt::transparent);
        {
            QPainter p(&image);
            p.setRenderHint(QPainter::Antialiasing);
            const int side = qMin(rect.width(), rect.height());
            p.translate((rect.width() - side) / 2.0, (rect.height() - side) / 2.0);
            p.scale(side / 24.0, side / 24.0);
            const QColor color = qApp->palette().color(mode == QIcon::Disabled ? QPalette::Disabled : QPalette::Active, QPalette::WindowText);
            p.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            QPainterPath bubble;
            bubble.moveTo(6, 4);
            bubble.lineTo(18, 4);
            bubble.quadTo(21, 4, 21, 7);
            bubble.lineTo(21, 14);
            bubble.quadTo(21, 17, 18, 17);
            bubble.lineTo(11, 17);
            bubble.lineTo(6.5, 21);
            bubble.lineTo(7, 17);
            bubble.lineTo(6, 17);
            bubble.quadTo(3, 17, 3, 14);
            bubble.lineTo(3, 7);
            bubble.quadTo(3, 4, 6, 4);
            p.drawPath(bubble);
            p.drawLine(QPointF(7.5, 9), QPointF(16.5, 9));
            p.drawLine(QPointF(7.5, 12.5), QPointF(13.5, 12.5));
            if (m_unread) {
                p.setCompositionMode(QPainter::CompositionMode_Clear);
                p.setPen(Qt::NoPen);
                p.setBrush(Qt::black);
                p.drawEllipse(QPointF(19.5, 4.5), 5.2, 5.2);
                p.setCompositionMode(QPainter::CompositionMode_SourceOver);
                p.setBrush(mode == QIcon::Disabled ? color : QColor(255, 59, 48));
                p.drawEllipse(QPointF(19.5, 4.5), 3.6, 3.6);
            }
        }
        painter->drawImage(rect, image);
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        QPixmap result(size);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        paint(&painter, QRect(QPoint(), size), mode, state);
        return result;
    }

private:
    bool m_unread = false;
};

// Removes every <Action name="name"/> directly inside container (a toolbar or a menu).
static void removeNamedActions(QDomElement &container, const QString &name)
{
    for (QDomElement action = container.firstChildElement(QStringLiteral("Action")); !action.isNull();) {
        const QDomElement next = action.nextSiblingElement(QStringLiteral("Action"));
        if (action.attribute(QStringLiteral("name")) == name) container.removeChild(action);
        action = next;
    }
}

static void installAuthoritativeMainToolbarConfig()
{
    constexpr int layoutVersion = 46;
    KConfigGroup migrationState = KSharedConfig::openConfig()->group(QStringLiteral("ThoughtsToolbar"));
    const int previousLayoutVersion = migrationState.readEntry(QStringLiteral("LayoutVersion"), 0);
    if (previousLayoutVersion >= layoutVersion) {
        return;
    }

    const QString configRoot = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).filePath(QStringLiteral("kxmlgui5/okular"));
    const QString shellPath = QDir(configRoot).filePath(QStringLiteral("shell.rc"));
    QFile shellFile(shellPath);
    if (!shellFile.exists()) {
        migrationState.writeEntry(QStringLiteral("LayoutVersion"), layoutVersion);
        migrationState.sync();
        return;
    }
    if (!shellFile.open(QIODevice::ReadOnly)) {
        return;
    }
    QString shellXml = QString::fromUtf8(shellFile.readAll());
    // Windows cannot replace a file that is still open.
    shellFile.close();
    const QRegularExpression toolbarExpression(QStringLiteral("<ToolBar\\b(?=[^>]*\\bname=[\"']mainToolBar[\"'])[^>]*>.*?</ToolBar>"), QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch toolbarMatch = toolbarExpression.match(shellXml);

    const QString authoritativeToolbar = QStringLiteral(
        "<ToolBar noMerge=\"1\" name=\"mainToolBar\">\n"
        "  <text>Main Toolbar</text>\n"
        "  <Action name=\"window_buttons_space\"/>\n"
        "  <Action name=\"sidebar_display\"/>\n"
        "  <Action name=\"mouse_textselect\"/>\n"
        "  <Action name=\"mouse_select\"/>\n"
        "  <Action name=\"mouse_draw\"/>\n"
        "  <Action name=\"mouse_text_box\"/>\n"
        "  <Action name=\"document_title\"/>\n"
        "  <Spacer/>\n"
        "  <Action name=\"go_document_back\"/>\n"
        "  <Action name=\"page_number\"/>\n"
        "  <Action name=\"go_document_forward\"/>\n"
        "  <Spacer/>\n"
        "  <Action name=\"zoom_to\"/>\n"
        "  <Action name=\"pdf_library_status\"/>\n"
        "  <Action name=\"comments_overview\"/>\n"
        "  <Action name=\"review_overview\"/>\n"
        "  <Action name=\"agent_notes\"/>\n"
        " </ToolBar>");
    if (toolbarMatch.hasMatch() && previousLayoutVersion < 32) {
        shellXml.replace(toolbarMatch.capturedStart(), toolbarMatch.capturedLength(), authoritativeToolbar);
    }

    QDomDocument document;
    if (!document.setContent(shellXml)) {
        return;
    }
    repairShellMenuLabelsDocument(document);
    if (previousLayoutVersion >= 32) {
        const QDomNodeList toolbars = document.elementsByTagName(QStringLiteral("ToolBar"));
        for (int index = 0; index < toolbars.size(); ++index) {
            QDomElement toolbar = toolbars.at(index).toElement();
            if (toolbar.attribute(QStringLiteral("name")) != QLatin1String("mainToolBar")) continue;
            QDomElement overview;
            QDomElement view;
            QDomElement highlights;
            bool hasReview = false;
            bool hasHighlights = false;
            bool hasSidebarDisplay = false;
            for (QDomElement action = toolbar.firstChildElement(QStringLiteral("Action")); !action.isNull(); action = action.nextSiblingElement(QStringLiteral("Action"))) {
                const QString name = action.attribute(QStringLiteral("name"));
                if (name == QLatin1String("comments_overview")) overview = action;
                if (name == QLatin1String("view_render_mode")) view = action;
                if (name == QLatin1String("view_highlights")) {
                    hasHighlights = true;
                    highlights = action;
                }
                if (name == QLatin1String("sidebar_display")) hasSidebarDisplay = true;
                if (name == QLatin1String("review_overview")) {
                    hasReview = true;
                }
            }
            if (!hasReview && previousLayoutVersion < 33) {
                QDomElement review = document.createElement(QStringLiteral("Action"));
                review.setAttribute(QStringLiteral("name"), QStringLiteral("review_overview"));
                if (overview.isNull()) toolbar.appendChild(review);
                else toolbar.insertAfter(review, overview);
            }
            // v37: the "Sidebar" chooser hides the sidebar too; its toggle icon goes,
            // with the separator after it. (v37, not v36: A/B testing had already
            // taken v36, so toolbars it migrated still had the toggle.)
            if (previousLayoutVersion < 37) {
                for (QDomElement action = toolbar.firstChildElement(QStringLiteral("Action")); !action.isNull(); action = action.nextSiblingElement(QStringLiteral("Action"))) {
                    if (action.attribute(QStringLiteral("name")) != QLatin1String("show_leftpanel")) continue;
                    const QDomElement next = action.nextSiblingElement();
                    if (next.tagName() == QLatin1String("Separator")) toolbar.removeChild(next);
                    toolbar.removeChild(action);
                    break;
                }
            }
            // v35: the "Sidebar" chooser replaces the View (page layout) menu,
            // which stays in the menu bar's View menu.
            if (previousLayoutVersion < 35 && !hasSidebarDisplay) {
                if (!view.isNull()) {
                    view.setAttribute(QStringLiteral("name"), QStringLiteral("sidebar_display"));
                } else {
                    QDomElement sidebarDisplay = document.createElement(QStringLiteral("Action"));
                    sidebarDisplay.setAttribute(QStringLiteral("name"), QStringLiteral("sidebar_display"));
                    if (highlights.isNull()) toolbar.appendChild(sidebarDisplay);
                    else toolbar.insertBefore(sidebarDisplay, highlights);
                }
            }
            // (v36 added an "A/B testing" button after Review; v46 removed it again.)
            // v38: "Visible markup" goes; the Annotations sidebar filters switch the
            // same Comments, Agent proposals and Review cards markup.
            if (previousLayoutVersion < 38 && hasHighlights) {
                toolbar.removeChild(highlights);
            }
            // v39: Back and Forward leave the far left for either side of the page
            // number, where the eye already is; the separator after them goes.
            if (previousLayoutVersion < 39) {
                QDomElement pageNumber;
                QDomElement back;
                QDomElement forward;
                for (QDomElement action = toolbar.firstChildElement(QStringLiteral("Action")); !action.isNull(); action = action.nextSiblingElement(QStringLiteral("Action"))) {
                    const QString name = action.attribute(QStringLiteral("name"));
                    if (name == QLatin1String("page_number")) pageNumber = action;
                    if (name == QLatin1String("go_document_back")) back = action;
                    if (name == QLatin1String("go_document_forward")) forward = action;
                }
                if (!pageNumber.isNull()) {
                    for (QDomElement history : {back, forward}) {
                        if (history.isNull()) continue;
                        const QDomElement next = history.nextSiblingElement();
                        if (next.tagName() == QLatin1String("Separator") && history.previousSiblingElement(QStringLiteral("Action")).isNull()) toolbar.removeChild(next);
                        toolbar.removeChild(history);
                    }
                    if (back.isNull()) {
                        back = document.createElement(QStringLiteral("Action"));
                        back.setAttribute(QStringLiteral("name"), QStringLiteral("go_document_back"));
                    }
                    if (forward.isNull()) {
                        forward = document.createElement(QStringLiteral("Action"));
                        forward.setAttribute(QStringLiteral("name"), QStringLiteral("go_document_forward"));
                    }
                    toolbar.insertBefore(back, pageNumber);
                    toolbar.insertAfter(forward, pageNumber);
                }
            }
            // v40: the text and rectangular selection tools sit right after the Sidebar chooser.
            if (previousLayoutVersion < 40) {
                QDomElement sidebarDisplay;
                QStringList present;
                for (QDomElement action = toolbar.firstChildElement(QStringLiteral("Action")); !action.isNull(); action = action.nextSiblingElement(QStringLiteral("Action"))) {
                    const QString name = action.attribute(QStringLiteral("name"));
                    if (name == QLatin1String("sidebar_display")) sidebarDisplay = action;
                    present.append(name);
                }
                QDomElement after = sidebarDisplay;
                for (const QString &name : {QStringLiteral("mouse_textselect"), QStringLiteral("mouse_select")}) {
                    if (present.contains(name)) continue;
                    QDomElement tool = document.createElement(QStringLiteral("Action"));
                    tool.setAttribute(QStringLiteral("name"), name);
                    if (after.isNull()) toolbar.appendChild(tool);
                    else toolbar.insertAfter(tool, after);
                    after = tool;
                }
            }
            // v41: one bar for the title and the tools. It starts with room for the window
            // buttons, the document title follows the selection tools, and Zoom leaves the far
            // left, where the window buttons now are, for the tools on the right.
            if (previousLayoutVersion < 41) {
                QDomElement windowButtons = document.createElement(QStringLiteral("Action"));
                windowButtons.setAttribute(QStringLiteral("name"), QStringLiteral("window_buttons_space"));
                toolbar.insertBefore(windowButtons, toolbar.firstChildElement(QStringLiteral("Action")));
                QDomElement selection;
                QDomElement zoom;
                QDomElement sync;
                bool hasTitle = false;
                for (QDomElement action = toolbar.firstChildElement(QStringLiteral("Action")); !action.isNull(); action = action.nextSiblingElement(QStringLiteral("Action"))) {
                    const QString name = action.attribute(QStringLiteral("name"));
                    if (name == QLatin1String("mouse_select")) selection = action;
                    if (name == QLatin1String("zoom_to")) zoom = action;
                    if (name == QLatin1String("pdf_library_status")) sync = action;
                    if (name == QLatin1String("document_title")) hasTitle = true;
                }
                if (!hasTitle) {
                    QDomElement title = document.createElement(QStringLiteral("Action"));
                    title.setAttribute(QStringLiteral("name"), QStringLiteral("document_title"));
                    if (selection.isNull()) toolbar.appendChild(title);
                    else toolbar.insertAfter(title, selection);
                }
                if (!zoom.isNull() && !sync.isNull()) {
                    toolbar.removeChild(zoom);
                    toolbar.insertBefore(zoom, sync);
                }
            }
            // v42: the pencil follows the two selection tools.
            if (previousLayoutVersion < 42) {
                QDomElement selection;
                bool hasDraw = false;
                for (QDomElement action = toolbar.firstChildElement(QStringLiteral("Action")); !action.isNull(); action = action.nextSiblingElement(QStringLiteral("Action"))) {
                    const QString name = action.attribute(QStringLiteral("name"));
                    if (name == QLatin1String("mouse_select")) selection = action;
                    if (name == QLatin1String("mouse_draw")) hasDraw = true;
                }
                if (!hasDraw && !selection.isNull()) {
                    QDomElement draw = document.createElement(QStringLiteral("Action"));
                    draw.setAttribute(QStringLiteral("name"), QStringLiteral("mouse_draw"));
                    toolbar.insertAfter(draw, selection);
                }
            }
            // v45: Agent Notes follows Review (else the Library button, else it ends the bar).
            if (previousLayoutVersion < 45) {
                QDomElement after;
                bool hasAgentNotes = false;
                for (QDomElement action = toolbar.firstChildElement(QStringLiteral("Action")); !action.isNull(); action = action.nextSiblingElement(QStringLiteral("Action"))) {
                    const QString name = action.attribute(QStringLiteral("name"));
                    if (name == QLatin1String("review_overview") || (after.isNull() && name == QLatin1String("comments_overview"))) after = action;
                    if (name == QLatin1String("agent_notes")) hasAgentNotes = true;
                }
                if (!hasAgentNotes) {
                    QDomElement notes = document.createElement(QStringLiteral("Action"));
                    notes.setAttribute(QStringLiteral("name"), QStringLiteral("agent_notes"));
                    if (after.isNull()) toolbar.appendChild(notes);
                    else toolbar.insertAfter(notes, after);
                }
            }
            // v44: the Text tool follows the pencil.
            if (previousLayoutVersion < 44) {
                QDomElement before;
                bool hasText = false;
                for (QDomElement action = toolbar.firstChildElement(QStringLiteral("Action")); !action.isNull(); action = action.nextSiblingElement(QStringLiteral("Action"))) {
                    const QString name = action.attribute(QStringLiteral("name"));
                    if (name == QLatin1String("mouse_draw") || (before.isNull() && name == QLatin1String("mouse_select"))) before = action;
                    if (name == QLatin1String("mouse_text_box")) hasText = true;
                }
                if (!hasText && !before.isNull()) {
                    QDomElement text = document.createElement(QStringLiteral("Action"));
                    text.setAttribute(QStringLiteral("name"), QStringLiteral("mouse_text_box"));
                    toolbar.insertAfter(text, before);
                }
            }
            // v46: the "A/B testing" button goes; the rating page it opened is no longer used.
            if (previousLayoutVersion < 46) removeNamedActions(toolbar, QStringLiteral("ab_testing"));
        }
    }
    const QDomElement menuBar = document.documentElement().firstChildElement(QStringLiteral("MenuBar"));
    for (QDomElement menu = menuBar.firstChildElement(QStringLiteral("Menu")); !menu.isNull(); menu = menu.nextSiblingElement(QStringLiteral("Menu"))) {
        if (menu.attribute(QStringLiteral("name")) != QLatin1String("file")) continue;
        if (previousLayoutVersion < 33) {
            QDomElement details = document.createElement(QStringLiteral("Action"));
            details.setAttribute(QStringLiteral("name"), QStringLiteral("pdf_sync_details"));
            menu.appendChild(details);
        }
        // v45: File > Agent Notes, after the review entry.
        if (previousLayoutVersion < 45) {
            QDomElement after;
            bool hasAgentNotes = false;
            for (QDomElement action = menu.firstChildElement(QStringLiteral("Action")); !action.isNull(); action = action.nextSiblingElement(QStringLiteral("Action"))) {
                const QString name = action.attribute(QStringLiteral("name"));
                if (name == QLatin1String("agent_notes")) hasAgentNotes = true;
                if (name == QLatin1String("review_overview")) after = action;
            }
            if (!hasAgentNotes && !after.isNull()) {
                QDomElement notes = document.createElement(QStringLiteral("Action"));
                notes.setAttribute(QStringLiteral("name"), QStringLiteral("agent_notes"));
                notes.setAttribute(QStringLiteral("append"), QStringLiteral("open_merge"));
                menu.insertAfter(notes, after);
            }
        }
        // v43: File > Copy for Agent, after the review entry.
        if (previousLayoutVersion < 43) {
            QDomElement after;
            bool hasCopy = false;
            for (QDomElement action = menu.firstChildElement(QStringLiteral("Action")); !action.isNull(); action = action.nextSiblingElement(QStringLiteral("Action"))) {
                const QString name = action.attribute(QStringLiteral("name"));
                if (name == QLatin1String("copy_for_agent")) hasCopy = true;
                if (name == QLatin1String("review_overview") || name == QLatin1String("ab_testing")) after = action;
            }
            if (!hasCopy) {
                QDomElement copy = document.createElement(QStringLiteral("Action"));
                copy.setAttribute(QStringLiteral("name"), QStringLiteral("copy_for_agent"));
                if (after.isNull()) menu.appendChild(copy);
                else menu.insertAfter(copy, after);
            }
        }
        // v46: File > A/B testing goes with its button.
        if (previousLayoutVersion < 46) removeNamedActions(menu, QStringLiteral("ab_testing"));
        break;
    }
    document.documentElement().setAttribute(QStringLiteral("version"), QString::number(layoutVersion));
    shellXml = QString::fromUtf8(document.toByteArray(1));

    const QString backupPath = shellPath + QStringLiteral(".pre-reader-toolbar-v41");
    if (!QFileInfo::exists(backupPath) && !QFile::copy(shellPath, backupPath)) {
        return;
    }
    QSaveFile savedShell(shellPath);
    savedShell.setDirectWriteFallback(false);
    const QByteArray encoded = shellXml.toUtf8();
    if (!savedShell.open(QIODevice::WriteOnly) || savedShell.write(encoded) != encoded.size() || !savedShell.commit()) {
        savedShell.cancelWriting();
        return;
    }
    migrationState.writeEntry(QStringLiteral("LayoutVersion"), layoutVersion);
    migrationState.sync();
}

bool Shell::migrateLegacyPdfLibraryActions(QDomDocument &document)
{
    QList<QDomElement> legacyActions;
    const QDomNodeList actions = document.elementsByTagName(QStringLiteral("Action"));
    for (int index = 0; index < actions.size(); ++index) {
        const QDomElement action = actions.at(index).toElement();
        if (action.attribute(QStringLiteral("name")) == QLatin1StringView("track_pdf")) {
            legacyActions.append(action);
        }
    }

    bool changed = false;
    for (QDomElement legacyAction : std::as_const(legacyActions)) {
        QDomNode parent = legacyAction.parentNode();
        if (parent.isNull()) {
            continue;
        }

        bool currentActionExists = false;
        for (QDomNode sibling = parent.firstChild(); !sibling.isNull(); sibling = sibling.nextSibling()) {
            const QDomElement siblingElement = sibling.toElement();
            if (!siblingElement.isNull() && siblingElement.tagName() == QLatin1StringView("Action") && siblingElement.attribute(QStringLiteral("name")) == QLatin1StringView("render_linked_pdf")) {
                currentActionExists = true;
                break;
            }
        }

        if (currentActionExists) {
            parent.removeChild(legacyAction);
        } else {
            legacyAction.setAttribute(QStringLiteral("name"), QStringLiteral("render_linked_pdf"));
        }
        changed = true;
    }
    return changed;
}

void Shell::migrateLegacyPdfLibraryActionConfig()
{
    const QString configRoot = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).filePath(QStringLiteral("kxmlgui5/okular"));
    const QString shellPath = QDir(configRoot).filePath(QStringLiteral("shell.rc"));
    QFile shellFile(shellPath);
    if (!shellFile.open(QIODevice::ReadOnly)) {
        return;
    }

    QDomDocument document;
    const bool parsed = static_cast<bool>(document.setContent(&shellFile));
    // Windows cannot replace a file that is still open.
    shellFile.close();
    if (!parsed || !migrateLegacyPdfLibraryActions(document)) {
        return;
    }

    const QString backupPath = shellPath + QStringLiteral(".pre-single-pdf-library-action-v1");
    if (!QFileInfo::exists(backupPath) && !QFile::copy(shellPath, backupPath)) {
        return;
    }

    QSaveFile savedShell(shellPath);
    savedShell.setDirectWriteFallback(false);
    const QByteArray encoded = document.toByteArray(1);
    if (!savedShell.open(QIODevice::WriteOnly) || savedShell.write(encoded) != encoded.size() || !savedShell.commit()) {
        savedShell.cancelWriting();
    }
}

// With @p moreAreas, the dialog also offers Add More Areas…, which accepts it and sets *moreAreas.
static QString promptManualReviewQuestion(QWidget *parent, bool *accepted, int *firstDueDays, QDateTime *customFirstDueAt,
                                          const QString &initialQuestion = {}, const QString &instructionText = {}, bool *moreAreas = nullptr)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(i18n("Add Review Card"));
    dialog.setModal(true);
    dialog.resize(480, 260);

    auto *layout = new QVBoxLayout(&dialog);
    auto *instructions = new QLabel(instructionText.isEmpty() ? i18n("Enter the question to show while the selected area is hidden.") : instructionText, &dialog);
    instructions->setWordWrap(true);
    layout->addWidget(instructions);

    auto *editor = new QPlainTextEdit(&dialog);
    editor->setObjectName(QStringLiteral("ManualReviewQuestion"));
    editor->setPlaceholderText(i18n("Question"));
    layout->addWidget(editor);

    auto *characterCount = new QLabel(i18n("0 / 500 characters"), &dialog);
    characterCount->setAlignment(Qt::AlignRight);
    layout->addWidget(characterCount);

    auto *dueRow = new QHBoxLayout;
    dueRow->addWidget(new QLabel(i18n("First review due:"), &dialog));
    auto *dueChoice = new QComboBox(&dialog);
    dueChoice->setObjectName(QStringLiteral("ManualReviewFirstDue"));
    dueChoice->addItem(i18n("Now"), 0);
    dueChoice->addItem(i18n("Tomorrow"), 1);
    dueChoice->addItem(i18n("In 2 days"), 2);
    dueChoice->addItem(i18n("In 1 week"), 7);
    dueChoice->addItem(i18n("In 30 days"), 30);
    dueChoice->addItem(i18n("Custom…"), -1);
    dueChoice->setCurrentIndex(2);
    dueRow->addWidget(dueChoice, 1);
    layout->addLayout(dueRow);

    auto *customDue = new QDateTimeEdit(QDateTime::currentDateTime().addDays(2), &dialog);
    customDue->setTime(QTime(customDue->time().hour(), customDue->time().minute()));
    customDue->setObjectName(QStringLiteral("ManualReviewCustomDue"));
    customDue->setAccessibleName(i18n("Custom first review due date and time"));
    customDue->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
    customDue->setCalendarPopup(true);
    customDue->setMinimumDateTime(QDateTime::currentDateTime().addSecs(60));
    customDue->setVisible(false);
    layout->addWidget(customDue);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QPushButton *addButton = buttons->button(QDialogButtonBox::Ok);
    addButton->setText(i18n("Add Card"));
    addButton->setEnabled(false);
    QPushButton *moreButton = nullptr;
    if (moreAreas) {
        *moreAreas = false;
        moreButton = buttons->addButton(i18n("Add More Areas…"), QDialogButtonBox::ActionRole);
        moreButton->setObjectName(QStringLiteral("ManualReviewMoreAreas"));
        moreButton->setToolTip(i18n("Choose further answer areas, on this page or others, before adding the card."));
        moreButton->setEnabled(false);
        QObject::connect(moreButton, &QPushButton::clicked, &dialog, [&dialog, moreAreas] {
            *moreAreas = true;
            dialog.accept();
        });
    }
    layout->addWidget(buttons);

    const auto update = [editor, characterCount, addButton, moreButton, dueChoice, customDue]() {
        const int length = editor->toPlainText().trimmed().size();
        characterCount->setText(i18n("%1 / 500 characters", length));
        const bool custom = dueChoice->currentData().toInt() == -1;
        customDue->setVisible(custom);
        addButton->setEnabled(length >= 1 && length <= 500 && (!custom || customDue->dateTime() > QDateTime::currentDateTime()));
        if (moreButton) moreButton->setEnabled(addButton->isEnabled());
    };
    QObject::connect(editor, &QPlainTextEdit::textChanged, &dialog, update);
    QObject::connect(dueChoice, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, update);
    QObject::connect(customDue, &QDateTimeEdit::dateTimeChanged, &dialog, update);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (!initialQuestion.isEmpty()) {
        editor->setPlainText(initialQuestion);
        editor->selectAll();
    }
    editor->setFocus();
    const bool wasAccepted = dialog.exec() == QDialog::Accepted;
    if (accepted) {
        *accepted = wasAccepted;
    }
    if (wasAccepted && firstDueDays) *firstDueDays = dueChoice->currentData().toInt();
    if (wasAccepted && customFirstDueAt && dueChoice->currentData().toInt() == -1) *customFirstDueAt = customDue->dateTime().toUTC();
    return wasAccepted ? editor->toPlainText().trimmed() : QString();
}

#ifdef Q_OS_WIN
// VS Code's command line on Windows is bin\code.cmd, which runs Code.exe as
// Node with its cli.js. Run the same two files directly: cmd.exe would
// reinterpret quotes and percent signs in the arguments (encoded URLs).
struct VisualStudioCodeCli {
    QString program;
    QString script;
};

static VisualStudioCodeCli visualStudioCodeCli()
{
    QStringList scripts;
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("code"));
    if (!onPath.isEmpty()) {
        scripts << onPath;
    }
    scripts << QDir::home().filePath(QStringLiteral("AppData/Local/Programs/Microsoft VS Code/bin/code.cmd"))
            << QDir(qEnvironmentVariable("ProgramFiles", QStringLiteral("C:/Program Files"))).filePath(QStringLiteral("Microsoft VS Code/bin/code.cmd"));
    for (const QString &scriptPath : std::as_const(scripts)) {
        QFile script(scriptPath);
        if (!scriptPath.endsWith(QLatin1String(".cmd"), Qt::CaseInsensitive) || !script.open(QIODevice::ReadOnly)) {
            continue;
        }
        // e.g. "%~dp0..\Code.exe" "%~dp0..\resources\app\out\cli.js" %*
        const QString text = QString::fromLocal8Bit(script.read(64 * 1024));
        const QDir bin = QFileInfo(scriptPath).absoluteDir();
        static const QRegularExpression quoted(QStringLiteral("\"%~dp0([^\"]+)\""));
        VisualStudioCodeCli cli;
        for (auto it = quoted.globalMatch(text); it.hasNext();) {
            const QString path = QDir::cleanPath(bin.filePath(QDir::fromNativeSeparators(it.next().captured(1))));
            if (path.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive) && cli.program.isEmpty()) {
                cli.program = path;
            } else if (path.endsWith(QLatin1String("cli.js"), Qt::CaseInsensitive) && cli.script.isEmpty()) {
                cli.script = path;
            }
        }
        if (cli.program.isEmpty()) {
            cli.program = QDir::cleanPath(bin.filePath(QStringLiteral("../Code.exe")));
        }
        if (cli.script.isEmpty()) {
            cli.script = QDir::cleanPath(bin.filePath(QStringLiteral("../resources/app/out/cli.js")));
        }
        if (QFileInfo(cli.program).isFile() && QFileInfo(cli.script).isFile()) {
            return cli;
        }
    }
    return {};
}
#endif

static QString visualStudioCodeExecutable()
{
#ifdef Q_OS_WIN
    return visualStudioCodeCli().program;
#else
    const QString pathExecutable = QStandardPaths::findExecutable(QStringLiteral("code"));
    if (!pathExecutable.isEmpty()) {
        return pathExecutable;
    }

    const QStringList candidates {
        QStringLiteral("/usr/local/bin/code"),
        QStringLiteral("/opt/homebrew/bin/code"),
#ifdef Q_OS_MACOS
        QStringLiteral("/Applications/Visual Studio Code.app/Contents/Resources/app/bin/code"),
#endif
    };
    for (const QString &candidate : candidates) {
        const QFileInfo info(candidate);
        if (info.isFile() && info.isExecutable()) {
            return info.absoluteFilePath();
        }
    }
    return {};
#endif
}

// Prepares process to run VS Code's command line with arguments.
static void configureVisualStudioCode(QProcess *process, const QString &executable, const QStringList &arguments)
{
    process->setProgram(executable);
#ifdef Q_OS_WIN
    const VisualStudioCodeCli cli = visualStudioCodeCli();
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("ELECTRON_RUN_AS_NODE"), QStringLiteral("1"));
    environment.remove(QStringLiteral("VSCODE_DEV"));
    process->setProcessEnvironment(environment);
    process->setArguments(QStringList {cli.script} + arguments);
#else
    process->setArguments(arguments);
#endif
}

static bool startVisualStudioCodeDetached(const QString &executable, const QStringList &arguments)
{
    QProcess process;
    configureVisualStudioCode(&process, executable, arguments);
    return process.startDetached();
}

static bool openSourceInVisualStudioCode(const QString &sourcePath, int line, QString *error)
{
    const QFileInfo source(sourcePath);
    if ((!source.isFile() && !(source.isDir() && line == 0)) || line < 0) {
        if (error) {
            *error = i18n("The recorded source location is unavailable.");
        }
        return false;
    }

    const QString executable = visualStudioCodeExecutable();
    if (executable.isEmpty()) {
        if (error) {
            *error = i18n("Visual Studio Code could not be found.");
        }
        return false;
    }

    const QStringList arguments = line > 0
        ? QStringList {QStringLiteral("--goto"), QStringLiteral("%1:%2:1").arg(source.canonicalFilePath()).arg(line)}
        : QStringList {source.canonicalFilePath()};
    if (!startVisualStudioCodeDetached(executable, arguments)) {
        if (error) {
            *error = i18n("Visual Studio Code could not be opened.");
        }
        return false;
    }
    return true;
}

// A conflict is reported by the publication that creates it; later Syncs
// stop earlier, while installing sources, with a generic "needs attention"
// result. Keep offering the resolution while the conflict is recorded.
static bool syncOperationOffersSourceConflictResolution(const QJsonObject &operation)
{
    return Thoughts::syncOperationReportsSourceConflict(operation)
        || (operation.value(QStringLiteral("status")).toString() == QLatin1String("failed") && !Thoughts::recordedSourceConflictPaths().isEmpty());
}

// Conflict IDs come from Fedora and name a workspace folder.
static bool safeSourceConflictComponent(const QString &conflictId)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]*$"));
    return pattern.match(conflictId).hasMatch() && !conflictId.contains(QStringLiteral(".."));
}

// On macOS a window-modal dialog is a sheet, and AppKit shows a hidden parent to hold it: the
// PDF window, hidden while the Library is in use, appeared as an empty page. Such dialogs block
// the whole application instead.
static Qt::WindowModality modalityOver(const QWidget *parent)
{
    return parent && parent->isVisible() ? Qt::WindowModal : Qt::ApplicationModal;
}

// Editable merge workspace for one conflict; Resolved.<extension> holds the user's merge.
static QString sourceConflictMergeDirectory(const QString &conflictId)
{
    return QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server/source-conflicts/%1/desktop-merge").arg(conflictId));
}

// The merge files keep the conflicted source's extension, so the editor highlights them correctly.
static QString sourceConflictSuffix(const QString &path)
{
    static const QRegularExpression plain(QStringLiteral("^[A-Za-z0-9]{1,12}$"));
    const QString suffix = QFileInfo(path).suffix();
    return plain.match(suffix).hasMatch() ? QLatin1Char('.') + suffix : QStringLiteral(".tex");
}

static QString sourceConflictResultPath(const QString &conflictId, const QString &path)
{
    const QDir root(sourceConflictMergeDirectory(conflictId));
    // A merge saved before the files took the source's extension stays where it is.
    const QString legacy = root.filePath(QStringLiteral("Resolved.tex"));
    return QFileInfo(legacy).isFile() ? legacy : root.filePath(QStringLiteral("Resolved") + sourceConflictSuffix(path));
}

static bool sourceConflictHasMarkers(const QByteArray &contents)
{
    static const QRegularExpression marker(QStringLiteral("(^|\\n)(<<<<<<<|=======|>>>>>>>)(?: |$)"));
    return marker.match(QString::fromUtf8(contents)).hasMatch();
}

static bool writeSourceConflictFile(const QString &path, const QByteArray &contents)
{
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size() && file.commit();
}

// A three-way merge keeps a line changed on only one side without a conflict.
// When one side decided a proposal (removing its whole wrapper) and the other
// still holds it, the conflict can cover only the wrapper's start: keeping this
// computer's half then leaves the wrapper open, and the PDF build fails on the
// rest of the file. Refuse a wrapper broken by the merge; one already broken
// the same way in a version being merged is that version's, not the merge's.
static QString sourceConflictProposalError(const QByteArray &contents, const QString &path, const QList<QByteArray> &versions)
{
    const Thoughts::AgentProposalFormat format = Thoughts::agentProposalFormat(path);
    if (format == Thoughts::AgentProposalFormat::Unsupported) {
        return {};
    }
    const Thoughts::AgentProposalParse resolved = Thoughts::parseAgentProposals(format, QString::fromUtf8(contents));
    if (resolved.issues.isEmpty()) {
        return {};
    }
    const auto issueKey = [](const Thoughts::AgentProposalIssue &issue) {
        return issue.code + QLatin1Char('\n') + (issue.hasId ? issue.id : QString());
    };
    QSet<QString> existing;
    for (const QByteArray &version : versions) {
        for (const Thoughts::AgentProposalIssue &issue : Thoughts::parseAgentProposals(format, QString::fromUtf8(version)).issues) {
            existing.insert(issueKey(issue));
        }
    }
    for (const Thoughts::AgentProposalIssue &issue : resolved.issues) {
        if (existing.contains(issueKey(issue))) {
            continue;
        }
        const QString proposal = issue.hasId && !issue.id.isEmpty() ? i18n("Proposal %1", issue.id) : i18n("A proposal");
        return i18n("%1 at line %2 is incomplete in the resolved file: %3 Keep the whole proposal from one version, or remove all of it.",
                    proposal, issue.line, Thoughts::agentProposalIssueMessage(issue));
    }
    return {};
}

static QString sourceConflictContentsError(const QByteArray &contents, const QString &path, const QList<QByteArray> &versions = {})
{
    if (contents.size() > 4 * 1024 * 1024) {
        return i18n("The resolved source exceeds the server’s 4 MiB limit.");
    }
    if (contents.contains('\0')) {
        return i18n("The resolved source contains a NUL byte.");
    }
    QStringDecoder decoder(QStringDecoder::Utf8);
    decoder.decode(contents);
    if (decoder.hasError()) {
        return i18n("The resolved source is not valid UTF-8.");
    }
    if (sourceConflictHasMarkers(contents)) {
        return i18n("The resolved source still contains merge markers.");
    }
    if (path.endsWith(QLatin1String(".json"), Qt::CaseInsensitive)) {
        // A settings file must stay readable: half of each side left in place breaks it.
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(contents, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            return i18n("The resolved file is not valid JSON: %1 (at character %2). Keep one complete version of each conflicting part.",
                        parseError.errorString(), parseError.offset);
        }
        if (!document.isObject()) {
            return i18n("The resolved file must contain one JSON object.");
        }
    }
    return sourceConflictProposalError(contents, path, versions);
}

QString Shell::sourceConflictContentsErrorForTesting(const QByteArray &contents, const QString &path, const QList<QByteArray> &versions)
{
    return sourceConflictContentsError(contents, path, versions);
}

static QList<QByteArray> sourceConflictVersions(const QVariantMap &bundle)
{
    return {QByteArray::fromBase64(bundle.value(QStringLiteral("fedora_content_base64")).toString().toLatin1()),
            QByteArray::fromBase64(bundle.value(QStringLiteral("mac_content_base64")).toString().toLatin1())};
}

QByteArray Shell::editableSourceConflictForTesting(const QByteArray &serverMerge,
                                                   const QByteArray &macContents,
                                                   const QByteArray &fedoraContents)
{
    if (serverMerge.isEmpty()) {
        QByteArray result("<<<<<<< THIS COMPUTER\n");
        result += macContents;
        if (!result.endsWith('\n')) {
            result += '\n';
        }
        result += "=======\n";
        result += fedoraContents;
        if (!result.endsWith('\n')) {
            result += '\n';
        }
        result += ">>>>>>> SERVER\n";
        return result;
    }

    const QList<QByteArray> lines = serverMerge.split('\n');
    QList<QByteArray> result;
    bool converted = false;
    for (qsizetype index = 0; index < lines.size(); ++index) {
        if (!lines.at(index).startsWith("<<<<<<<")) {
            result.append(lines.at(index));
            continue;
        }
        qsizetype separator = index + 1;
        while (separator < lines.size() && lines.at(separator) != QByteArrayLiteral("=======")) {
            ++separator;
        }
        qsizetype end = separator + 1;
        while (end < lines.size() && !lines.at(end).startsWith(">>>>>>>")) {
            ++end;
        }
        if (separator >= lines.size() || end >= lines.size()) {
            return {};
        }

        // Fedora generated the server merge as current/base/incoming, where
        // current is Fedora and incoming is this computer. Present this computer first so VS
        // Code's ordinary conflict actions match the labels shown to users.
        result.append(QByteArrayLiteral("<<<<<<< THIS COMPUTER"));
        for (qsizetype line = separator + 1; line < end; ++line) {
            result.append(lines.at(line));
        }
        result.append(QByteArrayLiteral("======="));
        for (qsizetype line = index + 1; line < separator; ++line) {
            result.append(lines.at(line));
        }
        result.append(QByteArrayLiteral(">>>>>>> SERVER"));
        index = end;
        converted = true;
    }
    return converted ? QByteArrayList(result.cbegin(), result.cend()).join('\n') : serverMerge;
}

static void openCodexForComment(QWidget *parent, const QString &pdfPath, const QString &annotationId)
{
    Thoughts::CommentCodexLauncher launcher(ShellUtils::docsRoot());
    QString error;
    if (!launcher.openComment(pdfPath, annotationId, &error)) {
        KMessageBox::error(parent, error, i18n("Open Codex"));
    }
}

static QString caseInsensitiveFile(const QDir &directory, const QString &fileName)
{
    const QFileInfoList entries = directory.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : entries) {
        if (entry.fileName().compare(fileName, Qt::CaseInsensitive) == 0) {
            return entry.absoluteFilePath();
        }
    }
    return {};
}

static QString caseInsensitiveDirectory(const QDir &directory, const QString &name)
{
    const QFileInfoList entries = directory.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : entries) {
        if (entry.fileName().compare(name, Qt::CaseInsensitive) == 0) {
            return entry.absoluteFilePath();
        }
    }
    return {};
}

QString Shell::newestArchivedSidecar(const QString &docsRoot, const QString &stem)
{
    const QDir archiveRoot(QDir(docsRoot).filePath(QStringLiteral("comments/.archive")));
    const QString stemDirectory = caseInsensitiveDirectory(archiveRoot, stem);
    if (stemDirectory.isEmpty()) {
        return {};
    }

    const QFileInfoList snapshots = QDir(stemDirectory).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
    for (const QFileInfo &snapshot : snapshots) {
        const QString sidecar = caseInsensitiveFile(QDir(snapshot.absoluteFilePath()), stem + QStringLiteral("_comments.jsonl"));
        if (!sidecar.isEmpty()) {
            return sidecar;
        }
    }
    return {};
}

bool Shell::restoreArchivedSidecar(const QString &archivedSidecar, const QString &destinationSidecar, QString *error)
{
    if (archivedSidecar.isEmpty()) {
        return true;
    }
    if (!QFile::rename(archivedSidecar, destinationSidecar)) {
        *error = i18n("The archived history could not be restored.");
        return false;
    }

    const QDir snapshotDirectory(QFileInfo(archivedSidecar).absolutePath());
    QFile::remove(snapshotDirectory.filePath(QStringLiteral("manifest.json")));
    const QString snapshotPath = snapshotDirectory.absolutePath();
    const QString stemPath = QFileInfo(snapshotPath).absolutePath();
    QDir().rmdir(snapshotPath);
    QDir().rmdir(stemPath);
    return true;
}

static inline QString DesktopEntryGroupKey()
{
    return QStringLiteral("Desktop Entry");
}
static inline QString RecentFilesGroupKey()
{
    return QStringLiteral("Recent Files");
}
static inline QString GeneralGroupKey()
{
    return QStringLiteral("General");
}

class ResizableStackedWidget : public QStackedWidget
{
    Q_OBJECT

public:
    QSize sizeHint() const override
    {
        return currentWidget()->sizeHint();
    }
    QSize minimumSizeHint() const override
    {
        return currentWidget()->minimumSizeHint();
    }
};

/**
 * Groups sidebar containers in a QDockWidget.
 *
 * This control groups all the sidebar containers provided by each tab (the Part object),
 * allowing the user to dock it to the left and right sides of the window,
 * or detach it from the window altogether.
 */
class Sidebar : public QDockWidget
{
    Q_OBJECT

public:
    explicit Sidebar(QWidget *parent = nullptr)
        : QDockWidget(parent)
    {
        setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
        setFeatures(defaultFeatures());

        m_stackedWidget = new QStackedWidget;
        setWidget(m_stackedWidget);
        // It seems that without requesting a specific minimum size, Qt
        // somehow calculates a (0,-1) minimum size, and then Qt gets angry
        // that negative sizes is not possible.
        setMinimumSize(10, 10);
    }

    bool isLocked() const
    {
        return features().testFlag(NoDockWidgetFeatures);
    }

    void setLocked(bool locked)
    {
        setFeatures(locked ? NoDockWidgetFeatures : defaultFeatures());

        // show titlebar only if not locked
        if (locked) {
            if (!m_dumbTitleWidget) {
                m_dumbTitleWidget = new QWidget;
            }
            setTitleBarWidget(m_dumbTitleWidget);
        } else {
            setTitleBarWidget(nullptr);
        }
    }

    int indexOf(QWidget *widget) const
    {
        return m_stackedWidget->indexOf(widget);
    }

    void addWidget(QWidget *widget)
    {
        m_stackedWidget->addWidget(widget);
    }

    void removeWidget(QWidget *widget)
    {
        m_stackedWidget->removeWidget(widget);
    }

    void setCurrentWidget(QWidget *widget)
    {
        m_stackedWidget->setCurrentWidget(widget);
    }

private:
    static DockWidgetFeatures defaultFeatures()
    {
        DockWidgetFeatures dockFeatures = DockWidgetClosable | DockWidgetMovable;
        if (!KWindowSystem::isPlatformWayland()) { // TODO : Remove this check when QTBUG-87332 is fixed
            dockFeatures |= DockWidgetFloatable;
        }

        return dockFeatures;
    }

    QStackedWidget *m_stackedWidget = nullptr;
    QWidget *m_dumbTitleWidget = nullptr;
};

class TabDropOverlay : public QWidget
{
public:
    explicit TabDropOverlay(QWidget *parent)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("tabDropOverlay"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        hide();
    }

    void setDropGeometry(const QRect &contentRect, const QRect &markerRect)
    {
        m_contentRect = contentRect;
        m_markerRect = markerRect;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        QColor highlight = palette().color(QPalette::Highlight);
        QColor fill = highlight;
        fill.setAlpha(28);
        highlight.setAlpha(190);

        painter.setBrush(fill);
        painter.setPen(QPen(highlight, 2));
        painter.drawRoundedRect(m_contentRect.adjusted(2, 2, -2, -2), 8, 8);

        if (m_markerRect.isValid()) {
            QColor marker = palette().color(QPalette::Highlight);
            marker.setAlpha(235);
            painter.setPen(Qt::NoPen);
            painter.setBrush(marker);
            painter.drawRoundedRect(m_markerRect, 2, 2);
        }
    }

private:
    QRect m_contentRect;
    QRect m_markerRect;
};

class TabDragPlaceholder : public QWidget
{
public:
    explicit TabDragPlaceholder(QWidget *parent)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("tabDragPlaceholder"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        QColor background = palette().color(QPalette::Window);
        background.setAlpha(170);
        QColor outline = palette().color(QPalette::Mid);
        outline.setAlpha(180);

        painter.setBrush(background);
        painter.setPen(QPen(outline, 1, Qt::DashLine));
        painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 5, 5);
    }
};

class TabDragCancelFilter : public QObject
{
public:
    bool canceled = false;

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if ((event->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) || event->type() == QEvent::ApplicationDeactivate) {
            canceled = true;
        }
        return QObject::eventFilter(object, event);
    }
};

struct ActiveTabDrag {
    QPointer<Shell> source;
    QPointer<QWidget> page;
    QPointer<Shell> emptiedSource;
    const QPixmap *pixmap = nullptr;
    QPoint hotSpot;
    bool transferred = false;
};

static ActiveTabDrag s_activeTabDrag;

QPointer<Thoughts::CommentsOverview> Shell::s_commentsOverview;
QPointer<Thoughts::AgentNotesWindow> Shell::s_agentNotesWindow;
QPointer<Shell> Shell::s_commentsOverviewShell;

Shell::Shell(const QString &serializedOptions)
    : Shell(nullptr, serializedOptions)
{
}

Shell::Shell(KParts::ReadWritePart *initialPart, const QString &serializedOptions)
    : KParts::MainWindow()
    , m_menuBarWasShown(true)
    , m_toolBarWasShown(true)
    , m_buildTerminalLauncher(std::make_unique<Thoughts::BuildTerminalLauncher>())
    , m_isValid(true)
{
    setObjectName(QStringLiteral("okular::Shell#"));
    startRemoteWorkWatch();
    startSyncJournalUpkeep();
    connect(qApp, &QGuiApplication::focusWindowChanged, this, [this] { styleMainToolBarActions(); });
    if (Thoughts::ReaderChrome::mergesTitleBar()) {
        setWindowFlag(Qt::ExpandedClientAreaHint);
        setWindowFlag(Qt::NoTitleBarBackgroundHint);
        setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
    }
    setContextMenuPolicy(Qt::NoContextMenu);
    // otherwise .rc file won't be found by unit test
    setComponentName(QStringLiteral("okular"), QString());
    migrateLegacyMainToolbarConfig();
    installAuthoritativeMainToolbarConfig();
    migrateLegacyPdfLibraryActionConfig();
    // set the shell's ui resource file
    setXMLFile(QStringLiteral("shell.rc"));
    m_fileformatsscanned = false;
    m_showMenuBarAction = nullptr;
    // this routine will find and load our Part.  it finds the Part by
    // name which is a bad idea usually.. but it's alright in this
    // case since our Part is made for this Shell

    const auto result = KPluginFactory::loadFactory(KPluginMetaData(QStringLiteral("kf6/parts/okularpart")));

    if (!result) {
        // if we couldn't find our Part, we exit since the Shell by
        // itself can't do anything useful
        m_isValid = false;
        KMessageBox::error(this, i18n("Unable to find the Thoughts component: %1", result.errorString));
        return;
    } else {
        m_partFactory = result.plugin;
    }

    // Reuse a transferred live Part when a tab becomes a window. This avoids
    // constructing and immediately destroying a blank placeholder document.
    KParts::ReadWritePart *const firstPart = initialPart ? initialPart : m_partFactory->create<KParts::ReadWritePart>(this);
    if (firstPart) {
        if (initialPart) {
            initialPart->setParent(this);
        }

        // Setup the central widget
        m_centralStackedWidget = new ResizableStackedWidget();
        setCentralWidget(m_centralStackedWidget);

        // Setup the welcome screen
        m_welcomeScreen = new WelcomeScreen(this);
        connect(m_welcomeScreen, &WelcomeScreen::openClicked, this, &Shell::fileOpen);
        connect(m_welcomeScreen, &WelcomeScreen::closeClicked, this, &Shell::hideWelcomeScreen);
        connect(m_welcomeScreen, &WelcomeScreen::recentItemClicked, this, [this](const QUrl &url) { openUrl(url); });
        connect(m_welcomeScreen, &WelcomeScreen::forgetRecentItem, this, &Shell::forgetRecentItem);
        m_centralStackedWidget->addWidget(m_welcomeScreen);

        m_welcomeScreen->installEventFilter(this);

        // Setup tab bar
        m_tabWidget = new DocumentTabWidget(this);
        m_tabWidget->setTabsClosable(true);
        m_tabWidget->setElideMode(Qt::ElideRight);
        m_tabWidget->tabBar()->hide();
        m_tabWidget->setDocumentMode(true);
        m_tabWidget->setMovable(true);
        m_tabWidget->tabBar()->setDrawBase(false);
        styleDocumentTabs();

        m_tabWidget->setAcceptDrops(true);
        m_tabWidget->installEventFilter(this);
        m_tabWidget->tabBar()->installEventFilter(this);
        m_tabWidget->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
        setAcceptDrops(true);

        m_centralStackedWidget->addWidget(m_tabWidget);

        connect(m_tabWidget, &QTabWidget::currentChanged, this, &Shell::setActiveTab);
        connect(m_tabWidget, &QTabWidget::tabCloseRequested, this, &Shell::closeTab);
        connect(m_tabWidget->tabBar(), &QTabBar::tabMoved, this, &Shell::moveTabData);
        m_pdfSourceFreshnessTimer = new QTimer(this);
        m_pdfSourceFreshnessTimer->setSingleShot(true);
        m_pdfSourceFreshnessTimer->setInterval(180);
        connect(m_pdfSourceFreshnessTimer, &QTimer::timeout, this, [this] { requestPdfLibraryStateRefresh(true); });
        // The open PDF's sources live in the docs root; the source manifest and
        // publication state live in the application data folder.
        for (const QString &root : {ShellUtils::docsRoot(),
                                    Thoughts::Platform::thoughtsDataRoot()}) {
            Thoughts::DocsChangeMonitor *monitor = Thoughts::DocsChangeMonitor::forRoot(root);
            connect(monitor, &Thoughts::DocsChangeMonitor::pathsChanged, this, &Shell::handlePdfSourcePathsChanged);
            connect(monitor, &Thoughts::DocsChangeMonitor::rescanRequired, this, [this] {
                if (!m_pdfSourceFreshnessPaths.isEmpty()) {
                    schedulePdfSourceFreshnessRefresh();
                }
            });
        }
        connect(m_tabWidget->tabBar(), &QWidget::customContextMenuRequested, this, [this](const QPoint &position) {
            const int tab = m_tabWidget->tabBar()->tabAt(position);
            if (tab < 0) {
                return;
            }

            QMenu menu(m_tabWidget->tabBar());
            menu.setObjectName(QStringLiteral("tabContextMenu"));
            addTabContextMenuActions(&menu, tab);
            menu.exec(m_tabWidget->tabBar()->mapToGlobal(position));
        });

        m_sidebar = new Sidebar;
        m_sidebar->setObjectName(QStringLiteral("okular_sidebar"));
        m_sidebar->setContextMenuPolicy(Qt::ActionsContextMenu);
        m_sidebar->setWindowTitle(i18n("Sidebar"));
        connect(m_sidebar, &QDockWidget::visibilityChanged, this, [this](bool visible) {
            // sync sidebar visibility with the m_showSidebarAction only if welcome screen is hidden
            if (m_showSidebarAction && m_centralStackedWidget->currentWidget() != m_welcomeScreen) {
                m_showSidebarAction->setChecked(visible);
            }
            if (m_centralStackedWidget->currentWidget() == m_welcomeScreen) {
                // MainWindow tries hard to make its child dockwidgets shown, but during
                // welcome screen we don't want to see the sidebar,
                // so try a bit more to actually hide it.
                m_sidebar->hide();
            }
        });
        addDockWidget(Qt::LeftDockWidgetArea, m_sidebar);

        // then, setup our actions
        setupActions();
        applyReaderIcons(actionCollection());
        m_activePartToolbar = std::make_unique<ActivePartToolbarController>(actionCollection(), firstPart->actionCollection());
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &QObject::deleteLater);
        // and integrate the part's GUI with the shell's
        setupGUI(Keys | ToolBar | Save);
        QAction *const pdfLibraryAction = actionCollection()->action(QStringLiteral("render_linked_pdf"));
        QMenu *fileMenu = nullptr;
        for (QAction *menuAction : menuBar()->actions()) {
            QMenu *const menu = menuAction->menu();
            if (menu && pdfLibraryAction && menu->actions().contains(pdfLibraryAction)) {
                fileMenu = menu;
                break;
            }
        }
        if (!fileMenu && factory()) {
            fileMenu = qobject_cast<QMenu *>(factory()->container(QStringLiteral("file"), nullptr));
        }
        QAction *const reviewAction = actionCollection()->action(QStringLiteral("review_overview"));
        if (fileMenu && reviewAction && !fileMenu->actions().contains(reviewAction)) {
            QAction *const undoCloseTab = actionCollection()->action(QStringLiteral("undo-close-tab"));
            if (undoCloseTab && fileMenu->actions().contains(undoCloseTab)) {
                fileMenu->insertAction(undoCloseTab, reviewAction);
            } else {
                fileMenu->addAction(reviewAction);
            }
        }
        // "Agent Notes" follows Review in the File menu.
        QAction *after = reviewAction;
        for (const QString &name : {QStringLiteral("agent_notes")}) {
            QAction *const action = actionCollection()->action(name);
            if (!fileMenu || !after || !action) break;
            if (!fileMenu->actions().contains(action)) {
                const QList<QAction *> fileActions = fileMenu->actions();
                const int afterIndex = fileActions.indexOf(after);
                if (afterIndex >= 0 && afterIndex + 1 < fileActions.size()) {
                    fileMenu->insertAction(fileActions.at(afterIndex + 1), action);
                } else {
                    fileMenu->addAction(action);
                }
            }
            after = action;
        }

        // Agent notes: one poller for the app; every window's button shows the unread dot.
        Thoughts::AgentNotes::Center *agentNotes = Thoughts::AgentNotes::Center::instance();
        connect(agentNotes, &Thoughts::AgentNotes::Center::unreadCountChanged, this, &Shell::updateAgentNotesAction);
        updateAgentNotesAction(agentNotes->hasListed() ? agentNotes->unreadCount() : 0);
        static bool agentNotesOpenConnected = false;
        if (!agentNotesOpenConnected) {
            agentNotesOpenConnected = true;
            connect(agentNotes, &Thoughts::AgentNotes::Center::openRequested, agentNotes, [](const QString &noteId) { Shell::showAgentNotes(noteId); });
        }
        // Tests never poll on their own: under test mode only an explicit request reaches a server.
        if (!QStandardPaths::isTestModeEnabled()) agentNotes->start();

        // NOTE : apply default sidebar width only after calling setupGUI(...)
        resizeDocks({m_sidebar}, {200}, Qt::Horizontal);

        m_tabs.append(TabState(firstPart));
        m_tabWidget->addTab(firstPart->widget(), QString()); // triggers setActiveTab that calls createGUI( part )

        connectPart(firstPart);
        m_readerDisplayTitles = new Thoughts::ReaderDisplayTitleStore(ShellUtils::docsRoot(), this, [this] { refreshReaderTabTitles(); });
        syncCommandService(true)->registerShell(this);

        readSettings();
        // A durable projection snapshot can predate connectPart(), so no change signal
        // is guaranteed on startup. Refresh the editor's review index from that snapshot.
        QTimer::singleShot(0, this, [this] { updateCommentsOverviewFedoraProjections(effectiveFedoraProjections(), 0); });

        m_unique = ShellUtils::unique(serializedOptions);
#if HAVE_DBUS
        if (m_unique) {
            m_unique = QDBusConnection::sessionBus().registerService(QStringLiteral("org.kde.okular"));
            if (!m_unique) {
                KMessageBox::information(this, i18n("There is already a unique Thoughts instance running. This instance won't be the unique one."));
            }
        } else {
            // TODO When porting to KF7 Remove
            // PID is not unique in containers and "-" in the name violates D-Bus naming conventions.
            // Was left for compatibility with 3rd-party scripts.
            QString serviceName = QStringLiteral("org.kde.okular-") + QString::number(qApp->applicationPid());
            QDBusConnection::sessionBus().registerService(serviceName);

            QDBusConnection::sessionBus().registerService(ShellUtils::currentProcessDbusName());
        }
        if (ShellUtils::noRaise(serializedOptions)) {
            setAttribute(Qt::WA_ShowWithoutActivating);
        }

        {
            const QString editorCmd = ShellUtils::editorCmd(serializedOptions);
            if (!editorCmd.isEmpty()) {
                QMetaObject::invokeMethod(firstPart, "setEditorCmd", Q_ARG(QString, editorCmd));
            }
        }

        QDBusConnection::sessionBus().registerObject(QStringLiteral("/okularshell"), this, QDBusConnection::ExportScriptableSlots);
#endif // HAVE_DBUS

        if (initialPart && !initialPart->url().isEmpty()) {
            m_centralStackedWidget->setCurrentWidget(m_tabWidget);
            m_tabWidget->tabBar()->setVisible(!Thoughts::ReaderChrome::mergesTitleBar());
            m_showSidebarAction->setEnabled(true);
            m_sidebar->setVisible(m_showSidebarAction->isChecked());
        } else {
            // Make sure that the welcome screen is visible on startup.
            showWelcomeScreen();
        }
    } else {
        m_isValid = false;
        KMessageBox::error(this, i18n("Unable to find the Thoughts component."));
    }

    connect(guiFactory(), &KXMLGUIFactory::shortcutsSaved, this, &Shell::reloadAllXML);
}

void Shell::reloadAllXML()
{
    for (const TabState &tab : std::as_const(m_tabs)) {
        tab.part->reloadXML();
    }
}

void Shell::saveNewToolbarConfig()
{
    KParts::MainWindow::saveNewToolbarConfig();
    materializeMainToolBar();
    scheduleMainToolBarMaterialization();
}

void Shell::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape && window()->isFullScreen()) {
        setFullScreen(false);
    }
}

static bool isActiveTabDrag(const QMimeData *mimeData)
{
    return mimeData && mimeData->hasFormat(QLatin1String(TAB_DRAG_MIME_TYPE)) && mimeData->data(QLatin1String(TAB_DRAG_MIME_TYPE)).toLongLong() == QCoreApplication::applicationPid() && s_activeTabDrag.source && s_activeTabDrag.page;
}

QPixmap Shell::tabDragPixmap(int tab) const
{
    if (tab < 0 || tab >= m_tabWidget->count()) {
        return {};
    }

    QTabBar *const tabBar = m_tabWidget->tabBar();
    const QRect tabRect = tabBar->tabRect(tab);
    const qreal devicePixelRatio = tabBar->devicePixelRatioF();

    QPixmap tabPixmap(tabRect.size() * devicePixelRatio);
    tabPixmap.setDevicePixelRatio(devicePixelRatio);
    tabPixmap.fill(Qt::transparent);
    {
        QPainter painter(&tabPixmap);
        tabBar->render(&painter, -tabRect.topLeft(), QRegion(tabRect), QWidget::DrawWindowBackground | QWidget::DrawChildren);
    }

    constexpr int shadowMargin = 8;
    const QSize ghostSize = tabRect.size() + QSize(shadowMargin * 2, shadowMargin * 2);
    QPixmap ghostPixmap(ghostSize * devicePixelRatio);
    ghostPixmap.setDevicePixelRatio(devicePixelRatio);
    ghostPixmap.fill(Qt::transparent);

    QPainter painter(&ghostPixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    for (int radius = 5; radius >= 1; --radius) {
        QColor shadow(Qt::black);
        shadow.setAlpha(7 + (5 - radius) * 3);
        painter.setPen(Qt::NoPen);
        painter.setBrush(shadow);
        painter.drawRoundedRect(QRectF(shadowMargin - radius / 2.0, shadowMargin + 3 - radius / 2.0, tabRect.width() + radius, tabRect.height() + radius), 6, 6);
    }
    painter.setOpacity(0.94);
    painter.drawPixmap(QPoint(shadowMargin, shadowMargin), tabPixmap);
    return ghostPixmap;
}

void Shell::startExternalTabDrag(QWidget *page)
{
    if (!page || s_activeTabDrag.source) {
        return;
    }

    const int tab = m_tabWidget->indexOf(page);
    if (tab < 0) {
        return;
    }

    constexpr int shadowMargin = 8;
    QPixmap dragPixmap = tabDragPixmap(tab);
    if (dragPixmap.isNull()) {
        return;
    }

    QTabBar *const tabBar = m_tabWidget->tabBar();
    const QRect sourceTabRect = tabBar->tabRect(tab);
    auto *placeholder = new TabDragPlaceholder(tabBar);
    placeholder->setGeometry(sourceTabRect);
    placeholder->show();
    placeholder->raise();

    const QPoint hotSpot(qBound(0, m_tabDragHotSpot.x(), sourceTabRect.width()) + shadowMargin, qBound(0, m_tabDragHotSpot.y(), sourceTabRect.height()) + shadowMargin);
    s_activeTabDrag = {this, page, nullptr, &dragPixmap, hotSpot, false};

    QDrag drag(tabBar);
    auto *mimeData = new QMimeData;
    mimeData->setData(QLatin1String(TAB_DRAG_MIME_TYPE), QByteArray::number(QCoreApplication::applicationPid()));
    drag.setMimeData(mimeData);
    drag.setPixmap(dragPixmap);
    drag.setHotSpot(hotSpot);

    TabDragCancelFilter cancelFilter;
    qApp->installEventFilter(&cancelFilter);
    m_draggedTabPage.clear();
    m_tabDragStarted = false;
    const Qt::DropAction result = drag.exec(Qt::MoveAction, Qt::MoveAction);
    qApp->removeEventFilter(&cancelFilter);

    const bool transferred = s_activeTabDrag.transferred;
    const QPointer<Shell> emptiedSource = s_activeTabDrag.emptiedSource;
    const QPoint dropPosition = QCursor::pos();
    s_activeTabDrag = {};
    delete placeholder;
    hideTabDropIndicator();

    if (!transferred && result == Qt::IgnoreAction && !cancelFilter.canceled && !frameGeometry().contains(dropPosition)) {
        const int currentTab = m_tabWidget->indexOf(page);
        if (currentTab >= 0) {
            detachTab(currentTab, dropPosition);
        }
    }

    // A live QDrag keeps its source QTabBar in use until QDrag itself is
    // destroyed at the end of this function. Deleting an emptied source
    // window from inside the nested drag event loop would destroy that tab
    // bar too early. Queue the deletion through the outer event loop instead.
    if (emptiedSource) {
        QTimer::singleShot(0, qApp, [emptiedSource] {
            if (emptiedSource) {
                emptiedSource->deleteLater();
            }
        });
    }
}

int Shell::tabInsertionIndex(const QPoint &globalPosition) const
{
    QTabBar *const tabBar = m_tabWidget->tabBar();
    const QPoint tabPosition = tabBar->mapFromGlobal(globalPosition);
    if (!tabBar->rect().contains(tabPosition)) {
        return m_tabWidget->count();
    }

    const int hoveredTab = tabBar->tabAt(tabPosition);
    if (hoveredTab < 0) {
        return m_tabWidget->count();
    }
    return tabPosition.x() < tabBar->tabRect(hoveredTab).center().x() ? hoveredTab : hoveredTab + 1;
}

void Shell::showTabDropIndicator(int insertionIndex)
{
    if (!m_tabDropOverlay) {
        m_tabDropOverlay = new TabDropOverlay(this);
    }

    m_tabDropOverlay->setGeometry(rect());
    const QPoint contentTopLeft = m_centralStackedWidget->mapTo(this, QPoint(0, 0));
    const QRect contentRect(contentTopLeft, m_centralStackedWidget->size());

    QRect markerRect;
    QTabBar *const tabBar = m_tabWidget->tabBar();
    if (tabBar->isVisible() && tabBar->count() > 0) {
        insertionIndex = qBound(0, insertionIndex, tabBar->count());
        int markerX;
        if (insertionIndex == tabBar->count()) {
            markerX = tabBar->tabRect(tabBar->count() - 1).right() + 1;
        } else {
            markerX = tabBar->tabRect(insertionIndex).left();
        }
        const QPoint markerTopLeft = tabBar->mapTo(this, QPoint(markerX - 2, 3));
        markerRect = QRect(markerTopLeft, QSize(4, qMax(4, tabBar->height() - 6)));
    }

    m_tabDropOverlay->setDropGeometry(contentRect, markerRect);
    m_tabDropOverlay->show();
    m_tabDropOverlay->raise();
}

void Shell::hideTabDropIndicator()
{
    if (m_tabDropOverlay) {
        m_tabDropOverlay->hide();
    }
}

bool Shell::handleTabDragMove(const QMimeData *mimeData, const QPoint &globalPosition)
{
    if (!isActiveTabDrag(mimeData)) {
        return false;
    }

    showTabDropIndicator(tabInsertionIndex(globalPosition));
    return true;
}

bool Shell::handleTabDrop(const QMimeData *mimeData, const QPoint &globalPosition)
{
    if (!isActiveTabDrag(mimeData)) {
        return false;
    }

    Shell *const source = s_activeTabDrag.source;
    QWidget *const page = s_activeTabDrag.page;
    const QPixmap dragPixmap = s_activeTabDrag.pixmap ? *s_activeTabDrag.pixmap : QPixmap();
    const QPoint hotSpot = s_activeTabDrag.hotSpot;
    const int insertionIndex = tabInsertionIndex(globalPosition);

    if (!source->transferTabTo(this, page, insertionIndex)) {
        hideTabDropIndicator();
        return false;
    }

    s_activeTabDrag.transferred = true;
    hideTabDropIndicator();
    const int finalTab = m_tabWidget->indexOf(page);
    if (finalTab >= 0) {
        animateTabDrop(dragPixmap, hotSpot, globalPosition, finalTab);
    }
    return true;
}

void Shell::animateTabDrop(const QPixmap &pixmap, const QPoint &hotSpot, const QPoint &globalPosition, int tab)
{
    if (pixmap.isNull() || tab < 0 || tab >= m_tabWidget->count()) {
        return;
    }

    auto *ghost = new QLabel(this);
    ghost->setAttribute(Qt::WA_TransparentForMouseEvents);
    ghost->setPixmap(pixmap);
    ghost->setScaledContents(false);
    ghost->resize(pixmap.deviceIndependentSize().toSize());
    ghost->move(mapFromGlobal(globalPosition - hotSpot));
    ghost->show();
    ghost->raise();

    auto *opacityEffect = new QGraphicsOpacityEffect(ghost);
    opacityEffect->setOpacity(0.92);
    ghost->setGraphicsEffect(opacityEffect);

    constexpr int shadowMargin = 8;
    const QPoint targetPosition = mapFromGlobal(m_tabWidget->tabBar()->mapToGlobal(m_tabWidget->tabBar()->tabRect(tab).topLeft())) - QPoint(shadowMargin, shadowMargin);

    auto *group = new QParallelAnimationGroup(ghost);
    auto *positionAnimation = new QPropertyAnimation(ghost, "pos", group);
    positionAnimation->setDuration(TAB_DROP_ANIMATION_DURATION);
    positionAnimation->setStartValue(ghost->pos());
    positionAnimation->setEndValue(targetPosition);
    positionAnimation->setEasingCurve(QEasingCurve::OutCubic);

    auto *opacityAnimation = new QPropertyAnimation(opacityEffect, "opacity", group);
    opacityAnimation->setDuration(TAB_DROP_ANIMATION_DURATION);
    opacityAnimation->setStartValue(0.92);
    opacityAnimation->setEndValue(0.0);
    opacityAnimation->setEasingCurve(QEasingCurve::OutCubic);

    connect(group, &QParallelAnimationGroup::finished, ghost, &QObject::deleteLater);
    group->start();
}

void Shell::animateWindowGeometry(const QRect &finalGeometry, const QPoint &dropPosition, bool fadeIn)
{
    const QRect startGeometry =
        fadeIn ? QRect(finalGeometry.center(), QSize(1, 1)).adjusted(-finalGeometry.width() * 48 / 100, -finalGeometry.height() * 48 / 100, finalGeometry.width() * 48 / 100, finalGeometry.height() * 48 / 100) : geometry();
    QRect animatedStart = startGeometry;
    if (fadeIn) {
        const QPoint subtleOffset((dropPosition.x() - finalGeometry.center().x()) / 12, (dropPosition.y() - finalGeometry.center().y()) / 12);
        animatedStart.translate(subtleOffset);
        setGeometry(animatedStart);
        setWindowOpacity(0.72);
        show();
    }

    auto *group = new QParallelAnimationGroup(this);
    auto *geometryAnimation = new QPropertyAnimation(this, "geometry", group);
    geometryAnimation->setDuration(TAB_DROP_ANIMATION_DURATION);
    geometryAnimation->setStartValue(fadeIn ? animatedStart : geometry());
    geometryAnimation->setEndValue(finalGeometry);
    geometryAnimation->setEasingCurve(QEasingCurve::OutCubic);

    if (fadeIn) {
        auto *opacityAnimation = new QPropertyAnimation(this, "windowOpacity", group);
        opacityAnimation->setDuration(TAB_DROP_ANIMATION_DURATION);
        opacityAnimation->setStartValue(0.72);
        opacityAnimation->setEndValue(1.0);
        opacityAnimation->setEasingCurve(QEasingCurve::OutCubic);
    }

    connect(group, &QParallelAnimationGroup::finished, this, [this, finalGeometry] {
        setGeometry(finalGeometry);
        setWindowOpacity(1.0);
    });
    group->start(QAbstractAnimation::DeleteWhenStopped);
}

void Shell::dragEnterEvent(QDragEnterEvent *event)
{
    if (handleTabDragMove(event->mimeData(), event->position().toPoint() + mapToGlobal(QPoint(0, 0)))) {
        event->setDropAction(Qt::MoveAction);
        event->accept();
        return;
    }
    KParts::MainWindow::dragEnterEvent(event);
}

void Shell::dragMoveEvent(QDragMoveEvent *event)
{
    if (handleTabDragMove(event->mimeData(), event->position().toPoint() + mapToGlobal(QPoint(0, 0)))) {
        event->setDropAction(Qt::MoveAction);
        event->accept();
        return;
    }
    KParts::MainWindow::dragMoveEvent(event);
}

void Shell::dragLeaveEvent(QDragLeaveEvent *event)
{
    hideTabDropIndicator();
    KParts::MainWindow::dragLeaveEvent(event);
}

void Shell::dropEvent(QDropEvent *event)
{
    const QPoint globalPosition = event->position().toPoint() + mapToGlobal(QPoint(0, 0));
    if (handleTabDrop(event->mimeData(), globalPosition)) {
        event->setDropAction(Qt::MoveAction);
        event->accept();
        return;
    }
    if (event->mimeData()->hasUrls()) {
        handleDroppedUrls(KUrlMimeData::urlsFromMimeData(event->mimeData()));
        event->acceptProposedAction();
        return;
    }
    KParts::MainWindow::dropEvent(event);
}

bool Shell::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == toolBar(QStringLiteral("mainToolBar"))) {
        const QEvent::Type eventType = event->type();
        // The document title stands for the tab: its right-click menu is the tab's (Copy for agent,
        // Copy project name…), not the toolbar's, also while the tab bar is hidden.
        if (eventType == QEvent::ContextMenu && m_documentTitle && m_documentTitle->isVisible() && m_tabWidget && m_tabWidget->currentIndex() >= 0) {
            const QPoint global = static_cast<QContextMenuEvent *>(event)->globalPos();
            if (m_documentTitle->rect().contains(m_documentTitle->mapFromGlobal(global))) {
                QMenu menu(m_documentTitle);
                menu.setObjectName(QStringLiteral("tabContextMenu"));
                addTabContextMenuActions(&menu, m_tabWidget->currentIndex());
                menu.exec(global);
                return true;
            }
        }
        // The pencil's button: its colours and eraser, not the toolbar's settings.
        if (eventType == QEvent::ContextMenu && showPencilMenu(static_cast<QContextMenuEvent *>(event)->globalPos())) {
            return true;
        }
        // The bar is the title bar too: its empty space moves and zooms the window.
        if (Thoughts::ReaderChrome::mergesTitleBar() && (eventType == QEvent::MouseButtonPress || eventType == QEvent::MouseButtonDblClick)
            && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
            if (eventType == QEvent::MouseButtonDblClick) {
                Thoughts::ReaderChrome::zoomWindow(this);
            } else if (windowHandle()) {
                windowHandle()->startSystemMove();
            }
            return true;
        }
        if (eventType == QEvent::ActionRemoved && !m_mainToolBarMaterializationInProgress && obj->isWidgetType()) {
            auto *mainToolBar = static_cast<QWidget *>(obj);
            if (mainToolBar->updatesEnabled()) {
                mainToolBar->setUpdatesEnabled(false);
                m_mainToolBarUpdatesSuppressed = true;
            }
        }
        if (eventType == QEvent::ActionAdded || eventType == QEvent::ActionRemoved) {
            scheduleMainToolBarMaterialization();
        }
        if (eventType == QEvent::Show || eventType == QEvent::StyleChange || eventType == QEvent::ActionAdded || eventType == QEvent::ActionRemoved || eventType == QEvent::ActionChanged) {
            QTimer::singleShot(0, this, &Shell::styleMainToolBarActions);
        }
        if (eventType == QEvent::LayoutRequest || eventType == QEvent::Resize || eventType == QEvent::Show || eventType == QEvent::ActionAdded || eventType == QEvent::ActionRemoved || eventType == QEvent::ActionChanged) {
            scheduleMainToolBarCentering();
        }
    }

    QDragMoveEvent *dmEvent = dynamic_cast<QDragMoveEvent *>(event);
    if (dmEvent) {
        QWidget *const targetWidget = qobject_cast<QWidget *>(obj);
        const QPoint globalPosition = targetWidget ? targetWidget->mapToGlobal(dmEvent->position().toPoint()) : QCursor::pos();
        if (handleTabDragMove(dmEvent->mimeData(), globalPosition)) {
            dmEvent->setDropAction(Qt::MoveAction);
            dmEvent->accept();
            return true;
        }

        const bool accept = dmEvent->mimeData()->hasUrls();
        event->setAccepted(accept);
        return accept;
    }

    QDropEvent *dEvent = dynamic_cast<QDropEvent *>(event);
    if (dEvent) {
        QWidget *const targetWidget = qobject_cast<QWidget *>(obj);
        const QPoint globalPosition = targetWidget ? targetWidget->mapToGlobal(dEvent->position().toPoint()) : QCursor::pos();
        if (handleTabDrop(dEvent->mimeData(), globalPosition)) {
            dEvent->setDropAction(Qt::MoveAction);
            dEvent->accept();
            return true;
        }

        if (!dEvent->mimeData()->hasUrls()) {
            return KParts::MainWindow::eventFilter(obj, event);
        }
        const QList<QUrl> list = KUrlMimeData::urlsFromMimeData(dEvent->mimeData());
        handleDroppedUrls(list);
        dEvent->setAccepted(true);
        return true;
    }

    if (event->type() == QEvent::DragLeave && s_activeTabDrag.source) {
        hideTabDropIndicator();
    }

    if (obj == m_tabWidget->tabBar()) {
        QMouseEvent *mouseEvent = dynamic_cast<QMouseEvent *>(event);
        if (mouseEvent && event->type() == QEvent::MouseButtonPress && mouseEvent->button() == Qt::LeftButton) {
            const int tabIndex = m_tabWidget->tabBar()->tabAt(mouseEvent->position().toPoint());
            m_draggedTabPage = tabIndex >= 0 ? m_tabWidget->widget(tabIndex) : nullptr;
            m_tabDragStartPosition = mouseEvent->globalPosition().toPoint();
            m_tabDragHotSpot = tabIndex >= 0 ? mouseEvent->position().toPoint() - m_tabWidget->tabBar()->tabRect(tabIndex).topLeft() : QPoint();
            m_tabDragStarted = false;
        } else if (mouseEvent && event->type() == QEvent::MouseMove && m_draggedTabPage && mouseEvent->buttons().testFlag(Qt::LeftButton)) {
            if ((mouseEvent->globalPosition().toPoint() - m_tabDragStartPosition).manhattanLength() >= QApplication::startDragDistance()) {
                m_tabDragStarted = true;
            }
            if (m_tabDragStarted && !m_tabWidget->tabBar()->rect().contains(mouseEvent->position().toPoint())) {
                const QPointer<QWidget> draggedTabPage = m_draggedTabPage;
                startExternalTabDrag(draggedTabPage);
                return true;
            }
        } else if (mouseEvent && event->type() == QEvent::MouseButtonRelease && mouseEvent->button() == Qt::LeftButton) {
            m_draggedTabPage.clear();
            m_tabDragStarted = false;
        }

        // Handle middle button click events on the tab bar
        if (mouseEvent && event->type() == QEvent::MouseButtonRelease && mouseEvent->button() == Qt::MiddleButton) {
            int tabIndex = m_tabWidget->tabBar()->tabAt(mouseEvent->position().toPoint());
            if (tabIndex != -1) {
                closeTab(tabIndex);
                return true;
            }
        }
    }
    return KParts::MainWindow::eventFilter(obj, event);
}

bool Shell::isValid() const
{
    return m_isValid;
}

void Shell::showOpenRecentMenu()
{
    m_recent->menu()->popup(QCursor::pos());
}

void Shell::copyTabForAgent(int tab)
{
    if (tab < 0 || tab >= m_tabs.size()) return;
    const QPointer<KParts::ReadWritePart> part = m_tabs.at(tab).part;
    const QUrl url = part ? part->url() : QUrl();
    if (!url.isLocalFile()) return;
    // The tab's complete document name, as Copy document name gives it.
    QString documentName = KLocalizedString::removeAcceleratorMarker(m_tabWidget->tabText(tab));
    // A page's tab: its document's name, as the document's tab gives it (the selection is the page's).
    if (isHtmlAttachmentTab(tab)) {
        const int document = findTabIndex(url);
        documentName = document >= 0 ? KLocalizedString::removeAcceleratorMarker(m_tabWidget->tabText(document)) : QString();
    }
    if (documentName.endsWith(QStringLiteral(" *"))) documentName.chop(2);
    if (documentName.isEmpty()) documentName = url.fileName();
    QString selection;
    if (part) {
        QMetaObject::invokeMethod(part, "selectedText", Qt::DirectConnection, Q_RETURN_ARG(QString, selection));
    }
    const QString root = ShellUtils::docsRoot();
    const QString pdfPath = url.toLocalFile();
    const QJsonArray projects = Thoughts::LibraryProjectStore(root).projects(false);
    QString text = Thoughts::agentDocumentDescription(root, projects, pdfPath, documentName, selection);
    QJsonObject projections = m_appliedFedoraProjections;
    if (part) QMetaObject::invokeMethod(part, "effectiveFedoraProjections", Qt::DirectConnection, Q_RETURN_ARG(QJsonObject, projections));
    if (projections.isEmpty()) projections = m_appliedFedoraProjections;
    const QString pdfSha256 = Thoughts::FeedbackVersion::cachedFileSha256(pdfPath);
    if (!pdfSha256.isEmpty() && !projections.isEmpty()) {
        const Thoughts::ReviewStore store(root, [projections] { return projections; });
        const QString stem = QFileInfo(pdfPath).completeBaseName();
        QList<Thoughts::ReviewCard> cards;
        for (const auto &card : store.cards(nullptr, stem)) {
            if (card.pdfStem() == stem) cards.append(card);
        }
        const QString documentId = Thoughts::agentProjectDocument(projects, QFileInfo(pdfPath).fileName())
                                       .second.value(QStringLiteral("document_id")).toString();
        const QList<QJsonObject> textFeedback = documentId.isEmpty() ? QList<QJsonObject>{} : store.documentFeedback(documentId);
        const QList<QJsonObject> answerFeedback = store.cardFeedback();
        const QString feedback = Thoughts::agentVersionFeedback(cards, answerFeedback, textFeedback, store.tagGroups(), pdfSha256);
        if (!feedback.isEmpty()) text += feedback;
    }
    QApplication::clipboard()->setText(text);
}

void Shell::addTabContextMenuActions(QMenu *menu, int tab)
{
    if (!menu || tab < 0 || tab >= m_tabs.size()) {
        return;
    }

    const QPointer<KParts::ReadWritePart> part = m_tabs.at(tab).part;
    const QPointer<QWidget> tabPage = m_tabWidget->widget(tab);
    const QUrl url = part->url();
    // A page's tab: its page and its document, none of the PDF's file actions.
    if (isHtmlAttachmentTab(tab)) {
        addHtmlAttachmentTabMenuActions(menu, tab);
        addTabWindowMenuActions(menu, tabPage);
        return;
    }
#ifdef Q_OS_WIN
    QAction *revealInFinderAction = menu->addAction(QIcon::fromTheme(QStringLiteral("document-open-folder")), i18n("Show in Explorer"));
#else
    QAction *revealInFinderAction = menu->addAction(QIcon::fromTheme(QStringLiteral("document-open-folder")), i18n("Reveal in Finder"));
#endif
    revealInFinderAction->setObjectName(QStringLiteral("revealInFinderAction"));
    revealInFinderAction->setData(url);
    revealInFinderAction->setEnabled(url.isLocalFile());
    connect(revealInFinderAction, &QAction::triggered, this, [this, part, tabPage, url]() {
        const QString localPath = url.toLocalFile();
        if (!QFileInfo::exists(localPath)) {
            const auto result = KMessageBox::questionTwoActions(this,
                                                                i18n("The original file no longer exists at “%1”. macOS may have removed it because it was opened as a temporary preview. Thoughts can still display the document, but Finder "
                                                                     "has no file to reveal.\n\nWould you like to export a permanent PDF copy?",
                                                                     localPath),
                                                                i18n("Temporary Preview File"),
                                                                KGuiItem(i18n("Export PDF Copy…"), QStringLiteral("document-export")),
                                                                KStandardGuiItem::cancel());
            if (result == KMessageBox::PrimaryAction && part && tabPage) {
                const int currentTab = m_tabWidget->indexOf(tabPage);
                if (currentTab >= 0) {
                    setActiveTab(currentTab);
                    if (QAction *exportAction = part->actionCollection()->action(QStringLiteral("file_export_pdf_with_comments"))) {
                        exportAction->trigger();
                    }
                }
            }
            return;
        }

#ifdef Q_OS_MACOS
        if (!QProcess::startDetached(QStringLiteral("/usr/bin/open"), {QStringLiteral("-R"), localPath})) {
            KMessageBox::error(this, i18n("Finder could not reveal “%1”.", localPath), i18n("Reveal in Finder Failed"));
        }
#elif defined(Q_OS_WIN)
        if (!Thoughts::Platform::revealInFileManager(localPath)) {
            KMessageBox::error(this, i18n("Explorer could not show “%1”.", localPath), i18n("Show in Explorer Failed"));
        }
#else
        KIO::highlightInFileManager({url});
#endif
    });

    QAction *copyDocumentNameAction = menu->addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy document name"));
    copyDocumentNameAction->setObjectName(QStringLiteral("copyDocumentNameAction"));
    // QTabBar elides the visible label but keeps the complete document name in
    // the tab data, so copy that instead of the truncated painting. KDE's
    // accelerator manager inserts '&' mnemonics into tab labels.
    QString documentName = KLocalizedString::removeAcceleratorMarker(m_tabWidget->tabText(tab));
    if (documentName.endsWith(QStringLiteral(" *"))) {
        documentName.chop(2);
    }
    if (documentName.isEmpty()) {
        documentName = url.fileName();
    }
    copyDocumentNameAction->setData(documentName);
    copyDocumentNameAction->setEnabled(!documentName.isEmpty());
    connect(copyDocumentNameAction, &QAction::triggered, this, [documentName]() {
        QApplication::clipboard()->setText(documentName);
    });

    // The Library project whose documents include this PDF, as the Library
    // tree shows it; a PDF outside every project has none to copy.
    const QString projectName = url.isLocalFile()
        ? Thoughts::LibraryProjectStore::projectNameForPdf(Thoughts::LibraryProjectStore(ShellUtils::docsRoot()).projects(false), url.fileName())
        : QString();
    QAction *copyProjectNameAction = menu->addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy project name"));
    copyProjectNameAction->setObjectName(QStringLiteral("copyProjectNameAction"));
    copyProjectNameAction->setData(projectName);
    copyProjectNameAction->setEnabled(!projectName.isEmpty());
    if (projectName.isEmpty()) {
        copyProjectNameAction->setToolTip(i18n("This document is not in a Library project."));
    }
    connect(copyProjectNameAction, &QAction::triggered, this, [projectName]() {
        QApplication::clipboard()->setText(projectName);
    });

    // Only documents imported from a link know where they came from: by this
    // computer, or by Fedora from a URL (an agent's import_pdf).
    const QUrl sourceLink = url.isLocalFile() ? Thoughts::LibraryProjectStore::sourceLinkForPdf(ShellUtils::docsRoot(), url.fileName()) : QUrl();
    if (!sourceLink.isEmpty()) {
        QAction *copyLinkAction = menu->addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy link"));
        copyLinkAction->setObjectName(QStringLiteral("copyLinkAction"));
        copyLinkAction->setData(sourceLink);
        connect(copyLinkAction, &QAction::triggered, this, [sourceLink]() {
            QApplication::clipboard()->setText(sourceLink.toString());
        });
    }

    // Paste into Codex or Claude: the document, its source and the reader's place, then the cards
    // reviewed and the text feedback given on the PDF version on screen.
    QAction *copyForAgentAction = menu->addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy for agent"));
    copyForAgentAction->setObjectName(QStringLiteral("copyForAgentAction"));
    copyForAgentAction->setEnabled(url.isLocalFile());
    connect(copyForAgentAction, &QAction::triggered, this, [this, tab]() { copyTabForAgent(tab); });

    addTabWindowMenuActions(menu, tabPage);
    // PDF | HTML for this document (thoughts-html-view-SPEC.md), remembered per document.
    bool htmlActive = false;
    if (part && url.isLocalFile() && QMetaObject::invokeMethod(part, "htmlViewActive", Qt::DirectConnection, Q_RETURN_ARG(bool, htmlActive))) {
        menu->addSeparator();
        QAction *viewAction = menu->addAction(QIcon::fromTheme(htmlActive ? QStringLiteral("application-pdf") : QStringLiteral("text-html")),
                                              htmlActive ? i18n("Show as PDF") : i18n("Show as HTML"));
        viewAction->setObjectName(QStringLiteral("documentViewTabAction"));
        connect(viewAction, &QAction::triggered, this, [part, htmlActive] {
            if (part) QMetaObject::invokeMethod(part, "setDocumentView", Qt::DirectConnection, Q_ARG(QString, htmlActive ? QStringLiteral("pdf") : QStringLiteral("html")));
        });
    }
}

void Shell::addTabWindowMenuActions(QMenu *menu, const QPointer<QWidget> &tabPage)
{
    menu->addSeparator();
    // The same as dragging the tab out of the window, for a window a little
    // below and to the right of this one.
    QAction *moveToNewWindowAction = menu->addAction(QIcon::fromTheme(QStringLiteral("window-new")), i18n("Move Tab to New Window"));
    moveToNewWindowAction->setObjectName(QStringLiteral("moveTabToNewWindowAction"));
    moveToNewWindowAction->setEnabled(m_tabs.size() > 1);
    connect(moveToNewWindowAction, &QAction::triggered, this, [this, tabPage]() {
        const int index = tabPage ? m_tabWidget->indexOf(tabPage) : -1;
        if (index >= 0) {
            detachTab(index, newWindowPosition());
        }
    });

    QAction *closeOtherTabsAction = menu->addAction(QIcon::fromTheme(QStringLiteral("tab-close")), i18n("Close Other Tabs"));
    closeOtherTabsAction->setObjectName(QStringLiteral("closeOtherTabsAction"));
    closeOtherTabsAction->setEnabled(m_tabs.size() > 1);
    connect(closeOtherTabsAction, &QAction::triggered, this, [this, tabPage]() {
        if (tabPage) {
            closeOtherTabs(tabPage);
        }
    });
}

void Shell::addHtmlAttachmentTabMenuActions(QMenu *menu, int tab)
{
    const QPointer<KParts::ReadWritePart> part = m_tabs.at(tab).part;
    const QUrl pdf = part->url();
    QAction *browserAction = menu->addAction(QIcon::fromTheme(QStringLiteral("internet-services")), i18n("Open in Browser"));
    browserAction->setObjectName(QStringLiteral("htmlPageOpenInBrowserAction"));
    connect(browserAction, &QAction::triggered, this, [part]() {
        if (part) QMetaObject::invokeMethod(part, "openHtmlAttachmentInBrowser", Qt::DirectConnection);
    });
    // Its document's tab, opened again when it was closed.
    QAction *documentAction = menu->addAction(QIcon::fromTheme(QStringLiteral("application-pdf")), i18n("Show Document"));
    documentAction->setObjectName(QStringLiteral("htmlPageShowDocumentAction"));
    connect(documentAction, &QAction::triggered, this, [this, pdf]() {
        const int document = findTabIndex(pdf);
        if (document >= 0) {
            setActiveTab(document);
        } else {
            openNewTab(pdf, QString());
        }
    });
    const QString name = KLocalizedString::removeAcceleratorMarker(m_tabWidget->tabText(tab));
    QAction *copyNameAction = menu->addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy page name"));
    copyNameAction->setObjectName(QStringLiteral("htmlPageCopyNameAction"));
    copyNameAction->setData(name);
    copyNameAction->setEnabled(!name.isEmpty());
    connect(copyNameAction, &QAction::triggered, this, [name]() { QApplication::clipboard()->setText(name); });
}

void Shell::closeOtherTabs(QWidget *tabPage)
{
    const int keepTab = m_tabWidget->indexOf(tabPage);
    if (keepTab < 0 || m_tabs.size() < 2) {
        return;
    }

    const QPointer<QWidget> previouslyActivePage = m_tabWidget->currentWidget();

    // Ask about every modified document before removing any tab. This keeps a
    // cancellation from leaving the window only partly closed.
    for (int tab = 0; tab < m_tabs.size(); ++tab) {
        if (m_tabWidget->widget(tab) == tabPage || !m_tabs.at(tab).part->isModified()) {
            continue;
        }

        setActiveTab(tab);
        if (!m_tabs.at(tab).part->queryClose()) {
            const int previousTab = m_tabWidget->indexOf(previouslyActivePage);
            if (previousTab >= 0) {
                setActiveTab(previousTab);
            }
            return;
        }
    }

    const int currentKeepTab = m_tabWidget->indexOf(tabPage);
    if (currentKeepTab < 0) {
        return;
    }
    setActiveTab(currentKeepTab);

    for (int tab = m_tabs.size() - 1; tab >= 0; --tab) {
        if (m_tabWidget->widget(tab) == tabPage) {
            continue;
        }
        if (!closeTabInternal(tab, false)) {
            break;
        }
    }
}

QPoint Shell::newWindowPosition() const
{
    // detachedWindowGeometry() centres the window horizontally on the point and
    // puts its tab bar there; offset the result from this window's top left.
    constexpr int cascadeOffset = 32;
    const QRect current = geometry();
    return QPoint(current.left() + cascadeOffset + current.width() / 2, current.top() + cascadeOffset + m_tabWidget->tabBar()->height() / 2);
}

QRect Shell::detachedWindowGeometry(const QPoint &globalPosition, const QSize &requestedSize) const
{
    QScreen *targetScreen = QGuiApplication::screenAt(globalPosition);
    if (!targetScreen) {
        targetScreen = screen();
    }
    if (!targetScreen) {
        targetScreen = QGuiApplication::primaryScreen();
    }

    const QRect availableGeometry = targetScreen->availableGeometry();
    QSize windowSize = requestedSize.expandedTo(minimumSizeHint());
    windowSize.setWidth(qMin(windowSize.width(), availableGeometry.width()));
    windowSize.setHeight(qMin(windowSize.height(), availableGeometry.height()));

    const QPoint desiredTopLeft(globalPosition.x() - windowSize.width() / 2, globalPosition.y() - m_tabWidget->tabBar()->height() / 2);
    const int maximumX = availableGeometry.right() - windowSize.width() + 1;
    const int maximumY = availableGeometry.bottom() - windowSize.height() + 1;
    const QPoint boundedTopLeft(qBound(availableGeometry.left(), desiredTopLeft.x(), maximumX), qBound(availableGeometry.top(), desiredTopLeft.y(), maximumY));
    return QRect(boundedTopLeft, windowSize);
}

void Shell::updateTabControls()
{
    const bool showingDocuments = m_centralStackedWidget->currentWidget() == m_tabWidget && !m_tabs.isEmpty();
    const bool multipleTabs = m_tabs.size() > 1;
    // With one document, the bar's title says it all.
    m_tabWidget->tabBar()->setVisible(showingDocuments && (multipleTabs || !Thoughts::ReaderChrome::mergesTitleBar()));

    m_nextTabAction->setEnabled(multipleTabs);
    m_prevTabAction->setEnabled(multipleTabs);
}

void Shell::removeEmptyPlaceholderPart()
{
    if (m_tabs.size() != 1 || !m_tabs.constFirst().part->url().isEmpty()) {
        return;
    }

    KParts::ReadWritePart *const placeholderPart = m_tabs.constFirst().part;
    Okular::ViewerInterface *placeholderInterface = qobject_cast<Okular::ViewerInterface *>(placeholderPart);
    Q_ASSERT(placeholderInterface);
    QWidget *const placeholderSideContainer = placeholderInterface->getSideContainer();

    if (m_showSidebarAction) {
        m_showSidebarAction->disconnect(m_sidebar);
        m_showSidebarAction = nullptr;
    }
    if (placeholderPart->factory()) {
        placeholderPart->factory()->removeClient(placeholderPart);
    }
    placeholderPart->disconnect(this);
    disconnect(this, nullptr, placeholderPart, nullptr);
    m_sidebar->removeWidget(placeholderSideContainer);

    {
        const QSignalBlocker blocker(m_tabWidget);
        m_tabWidget->removeTab(0);
        m_tabs.clear();
    }

    connect(placeholderPart, &QObject::destroyed, placeholderSideContainer, &QObject::deleteLater);
    placeholderPart->deleteLater();
}

Shell::TransferredTab Shell::takeTab(int tab)
{
    TransferredTab transferredTab;
    if (tab < 0 || tab >= m_tabs.size()) {
        return transferredTab;
    }

    const int previousActiveTab = m_tabWidget->currentIndex();
    const TabState tabState = m_tabs.at(tab);
    transferredTab.part = tabState.part;
    transferredTab.printEnabled = tabState.printEnabled;
    transferredTab.closeEnabled = tabState.closeEnabled;
    transferredTab.text = m_tabWidget->tabText(tab);
    transferredTab.toolTip = m_tabWidget->tabToolTip(tab);
    transferredTab.icon = m_tabWidget->tabIcon(tab);
    transferredTab.sidebarVisible = m_sidebar->isVisibleTo(this) || (m_showSidebarAction && m_showSidebarAction->isChecked());

    if (m_showSidebarAction == transferredTab.part->actionCollection()->action(QStringLiteral("show_leftpanel"))) {
        m_showSidebarAction->disconnect(m_sidebar);
        m_showSidebarAction = nullptr;
    }
    if (transferredTab.part->factory()) {
        transferredTab.part->factory()->removeClient(transferredTab.part);
    }
    forgetNavigationHistory(transferredTab.part);
    transferredTab.part->disconnect(this);
    disconnect(this, nullptr, transferredTab.part, nullptr);

    Okular::ViewerInterface *partInterface = qobject_cast<Okular::ViewerInterface *>(transferredTab.part);
    Q_ASSERT(partInterface);
    m_sidebar->removeWidget(partInterface->getSideContainer());

    {
        const QSignalBlocker blocker(m_tabWidget);
        m_tabWidget->removeTab(tab);
        m_tabs.removeAt(tab);
    }

    if (!m_tabs.isEmpty()) {
        const int nextActiveTab = previousActiveTab == tab ? qMin(tab, m_tabs.size() - 1) : previousActiveTab - (previousActiveTab > tab ? 1 : 0);
        setActiveTab(nextActiveTab);
        updateTabControls();
    } else {
        m_sidebar->hide();
        m_tabWidget->tabBar()->hide();
        m_nextTabAction->setEnabled(false);
        m_prevTabAction->setEnabled(false);
    }

    return transferredTab;
}

void Shell::applyTransferredTabMetadata(int tab, const TransferredTab &transferredTab)
{
    Q_ASSERT(tab >= 0 && tab < m_tabs.size());
    m_tabs[tab].printEnabled = transferredTab.printEnabled;
    m_tabs[tab].closeEnabled = transferredTab.closeEnabled;
    m_tabWidget->setTabText(tab, transferredTab.text);
    m_tabWidget->setTabToolTip(tab, transferredTab.toolTip);
    m_tabWidget->setTabIcon(tab, transferredTab.icon);

    m_showSidebarAction->setEnabled(true);
    m_showSidebarAction->setChecked(transferredTab.sidebarVisible);
    m_sidebar->setVisible(transferredTab.sidebarVisible);
    setCaption(transferredTab.text);
}

bool Shell::insertTransferredTab(TransferredTab transferredTab, int insertionIndex)
{
    if (!transferredTab.part) {
        return false;
    }

    removeEmptyPlaceholderPart();
    insertionIndex = qBound(0, insertionIndex, m_tabs.size());
    transferredTab.part->setParent(this);

    TabState tabState(transferredTab.part);
    tabState.printEnabled = transferredTab.printEnabled;
    tabState.closeEnabled = transferredTab.closeEnabled;

    {
        const QSignalBlocker blocker(m_tabWidget);
        m_tabs.insert(insertionIndex, tabState);
        m_tabWidget->insertTab(insertionIndex, transferredTab.part->widget(), transferredTab.icon, transferredTab.text);
        m_tabWidget->setTabToolTip(insertionIndex, transferredTab.toolTip);
    }

    connectPart(transferredTab.part);
    m_centralStackedWidget->setCurrentWidget(m_tabWidget);
    updateTabControls();
    setActiveTab(insertionIndex);
    applyTransferredTabMetadata(insertionIndex, transferredTab);
    transferredTab.part->widget()->setFocus();
    return true;
}

bool Shell::transferTabTo(Shell *destination, QWidget *page, int insertionIndex)
{
    if (!destination || !page) {
        return false;
    }

    const int sourceIndex = m_tabWidget->indexOf(page);
    if (sourceIndex < 0) {
        return false;
    }

    if (destination == this) {
        int finalIndex = qBound(0, insertionIndex, m_tabs.size());
        if (finalIndex > sourceIndex) {
            --finalIndex;
        }
        finalIndex = qBound(0, finalIndex, m_tabs.size() - 1);
        if (finalIndex != sourceIndex) {
            m_tabWidget->tabBar()->moveTab(sourceIndex, finalIndex);
        }
        setActiveTab(finalIndex);
        return true;
    }

    TransferredTab transferredTab = takeTab(sourceIndex);
    if (!transferredTab.part) {
        return false;
    }

    if (!destination->insertTransferredTab(transferredTab, insertionIndex)) {
        insertTransferredTab(transferredTab, qMin(sourceIndex, m_tabs.size()));
        return false;
    }

    if (m_tabs.isEmpty()) {
        hide();
        if (s_activeTabDrag.source == this) {
            s_activeTabDrag.emptiedSource = this;
        } else {
            deleteLater();
        }
    }
    return true;
}

void Shell::detachTab(int tab, const QPoint &globalPosition)
{
    if (tab < 0 || tab >= m_tabs.size()) {
        return;
    }

    const QRect newWindowGeometry = detachedWindowGeometry(globalPosition, size());
    if (m_tabs.size() == 1) {
        animateWindowGeometry(newWindowGeometry, globalPosition, false);
        raise();
        activateWindow();
        return;
    }

    TransferredTab transferredTab = takeTab(tab);
    if (!transferredTab.part) {
        return;
    }

    Shell *const detachedShell = new Shell(transferredTab.part, QString());
    if (!detachedShell->isValid()) {
        insertTransferredTab(transferredTab, qMin(tab, m_tabs.size()));
        delete detachedShell;
        return;
    }

    detachedShell->applyTransferredTabMetadata(0, transferredTab);
    detachedShell->updateTabControls();
    detachedShell->animateWindowGeometry(newWindowGeometry, globalPosition, true);
    detachedShell->raise();
    detachedShell->activateWindow();
}

Shell::~Shell()
{
    // Destroying the base window deletes the toolbars and then the native
    // window, which changes the focus window. Application-wide signals must not
    // reach this half-destroyed Shell: focusWindowChanged restyled the deleted
    // Sync button (use after free while closing a window).
    disconnect(qApp, nullptr, this, nullptr);
    // Child processes (such as a Visual Studio Code merge waiting for its
    // editor) are killed when the window goes away. Their error handlers
    // must not open dialogs on a window that is being destroyed.
    for (QProcess *process : findChildren<QProcess *>()) {
        disconnect(process, nullptr, this, nullptr);
    }
    flushPendingSyncTransferProgress();
    if (ThoughtsSyncCommandService *service = syncCommandService(false)) {
        service->unregisterShell(this);
    }
    if (!m_tabs.empty()) {
        writeSettings();
        for (const TabState &tab : std::as_const(m_tabs)) {
            tab.part->closeUrl(false);
        }
        m_tabs.clear();
    }
#if HAVE_DBUS
    if (m_unique) {
        QDBusConnection::sessionBus().unregisterService(QStringLiteral("org.kde.okular"));
    }
#endif // HAVE_DBUS

    delete m_tabWidget;
}

// Open a new document if we have space for it
// This can hang if called on a unique instance and openUrl pops a messageBox
bool Shell::openDocument(const QUrl &url, const QString &serializedOptions)
{
    if (m_tabs.size() <= 0) {
        return false;
    }

    hideWelcomeScreen();

    KParts::ReadWritePart *const part = m_tabs[0].part;

    // Return false if we can't open new tabs and the only part is occupied
    if (!qobject_cast<Okular::ViewerInterface *>(part)->openNewFilesInTabs() && !part->url().isEmpty() && !ShellUtils::unique(serializedOptions)) {
        return false;
    }

    openUrl(url, serializedOptions);

    return true;
}

bool Shell::openDocument(const QString &urlString, const QString &serializedOptions)
{
    return openDocument(QUrl(urlString), serializedOptions);
}

void Shell::openNewlySignedFile(const QString &path, int pageNumber)
{
    // for now, this function just applies the "replace current document"
    // strategy for opening.
    // Given signing is slightly closer to annotating and saving, the over
    // all user experience should not be that different
    // The fact that we get a different file out of saving is a bit of a
    // implementation  detail that shouldn't leak that much onto the users
    QUrl url = QUrl::fromLocalFile(path);
    url.setFragment(QStringLiteral("page=%1").arg(pageNumber));

    const int activeTab = m_tabWidget->currentIndex();
    KParts::ReadWritePart *const activePart = m_tabs[activeTab].part;
    activePart->closeUrl(false);
    activePart->openUrl(url);
}

bool Shell::canOpenDocs(int numDocs, int desktop)
{
    if (m_tabs.size() <= 0 || numDocs <= 0 || m_unique) {
        return false;
    }

    KParts::ReadWritePart *const part = m_tabs[0].part;
    const bool allowTabs = qobject_cast<Okular::ViewerInterface *>(part)->openNewFilesInTabs();

    if (!allowTabs && (numDocs > 1 || !part->url().isEmpty())) {
        return false;
    }

#if !defined(Q_OS_WIN) && !defined(Q_OS_OSX) && !defined(Q_OS_HAIKU)
    const KWindowInfo winfo(window()->effectiveWinId(), NET::WMDesktop);
    if (winfo.desktop() != desktop) {
        return false;
    }
#else
    Q_UNUSED(desktop);
#endif

    return true;
}

void Shell::openUrl(const QUrl &url, const QString &serializedOptions)
{
    const Thoughts::DeveloperActivity activity(QStringLiteral("document.open"), {{QStringLiteral("document_id"), Thoughts::DeveloperLog::documentId(url.toString())}});
    hideWelcomeScreen();

    const int activeTab = m_tabWidget->currentIndex();
    KParts::ReadWritePart *const activePart = m_tabs[activeTab].part;
    if (!activePart->url().isEmpty()) {
        if (m_unique && !isHtmlAttachmentTab(activeTab)) {
            applyOptionsToPart(activePart, serializedOptions);
            activePart->openUrl(url);
            updatePdfLibraryAction();
        } else {
            // A page's tab is never reused for a document.
            if (m_unique || qobject_cast<Okular::ViewerInterface *>(activePart)->openNewFilesInTabs()) {
                openNewTab(url, serializedOptions);
            } else {
                Shell *newShell = new Shell(serializedOptions);
                newShell->show();
                newShell->openUrl(url, serializedOptions);
            }
        }
    } else {
        m_tabWidget->setTabText(activeTab, url.fileName());
        m_tabWidget->setTabToolTip(activeTab, url.fileName());

        applyOptionsToPart(activePart, serializedOptions);
        bool openOk = activePart->openUrl(url);
        updatePdfLibraryAction();
        const bool isstdin = url.fileName() == QLatin1String("-") || url.scheme() == QLatin1String("fd");
        if (!isstdin) {
            if (openOk) {
                m_recent->addUrl(url);
            } else {
                m_recent->removeUrl(url);
                closeTab(activeTab);
            }
        }
    }

    if (!isVisible() && hasOpenDocument() && s_commentsOverviewShell == this && s_commentsOverview && s_commentsOverview->isVisible()) {
        s_commentsOverview->hide();
    }
}

void Shell::closeUrl()
{
    closeTab(m_tabWidget->currentIndex());

    // When closing the current tab two things can happen:
    //  * the focus was on the tab
    //  * the focus was somewhere in the toolbar
    // we don't have other places that accept focus
    //  * If it was on the tab, logic says it should go back to the next current tab
    //  * If it was on the toolbar, we could leave it there, but since we redo the menus/toolbars for the new tab, it gets kind of lost
    //    so it's easier to set it to the next current tab which also makes sense as consistency
    if (m_tabWidget->count() >= 0) {
        KParts::ReadWritePart *const newPart = m_tabs[m_tabWidget->currentIndex()].part;
        newPart->widget()->setFocus();
    }
}

void Shell::readSettings()
{
    readRecentFilesSettings();

    const KConfigGroup group = KSharedConfig::openConfig()->group(DesktopEntryGroupKey());
    bool fullScreen = group.readEntry("FullScreen", false);
    setFullScreen(fullScreen);

    if (fullScreen) {
        m_menuBarWasShown = group.readEntry(shouldShowMenuBarComingFromFullScreen, true);
        m_toolBarWasShown = group.readEntry(shouldShowToolBarComingFromFullScreen, true);
    }

    const KConfigGroup sidebarGroup = KSharedConfig::openConfig()->group(GeneralGroupKey());
    m_sidebar->setVisible(sidebarGroup.readEntry(SIDEBAR_VISIBLE_KEY, true));
    m_sidebar->setLocked(sidebarGroup.readEntry(SIDEBAR_LOCKED_KEY, true));

    m_showSidebarAction->setChecked(m_sidebar->isVisibleTo(this));
    m_lockSidebarAction->setChecked(m_sidebar->isLocked());
}

void Shell::writeSettings()
{
    saveRecents();

    KConfigGroup sidebarGroup = KSharedConfig::openConfig()->group(GeneralGroupKey());
    sidebarGroup.writeEntry(SIDEBAR_LOCKED_KEY, m_sidebar->isLocked());
    // NOTE : Consider whether the m_showSidebarAction is checked, because
    // the sidebar can be forcibly hidden if the welcome screen is displayed
    sidebarGroup.writeEntry(SIDEBAR_VISIBLE_KEY, m_sidebar->isVisibleTo(this) || m_showSidebarAction->isChecked());

    KConfigGroup group = KSharedConfig::openConfig()->group(DesktopEntryGroupKey());
    group.writeEntry("FullScreen", m_fullScreenAction->isChecked());
    if (m_fullScreenAction->isChecked()) {
        group.writeEntry(shouldShowMenuBarComingFromFullScreen, m_menuBarWasShown);
        group.writeEntry(shouldShowToolBarComingFromFullScreen, m_toolBarWasShown);
    }
    KSharedConfig::openConfig()->sync();
}

void Shell::saveRecents()
{
    m_recent->saveEntries(KSharedConfig::openConfig()->group(RecentFilesGroupKey()));
}

void Shell::setupActions()
{
    KStandardAction::open(this, SLOT(fileOpen()), actionCollection());
    QAction *openFromUrlAction = actionCollection()->addAction(QStringLiteral("open_from_url"));
    openFromUrlAction->setText(i18n("Open from URL…"));
    openFromUrlAction->setIcon(QIcon::fromTheme(QStringLiteral("document-open")));
    openFromUrlAction->setToolTip(i18n("Open a document from an HTTP or HTTPS URL"));
    connect(openFromUrlAction, &QAction::triggered, this, &Shell::openFromUrl);
    m_pdfLibraryAction = actionCollection()->addAction(QStringLiteral("render_linked_pdf"));
    m_pdfLibraryAction->setText(i18n("Manage Current PDF"));
    m_pdfLibraryAction->setIconText(i18n("Add to Library…"));
    m_pdfLibraryAction->setIcon(QIcon::fromTheme(QStringLiteral("document-import")));
    m_pdfLibraryAction->setToolTip(i18n("Choose a PDF to add to the shared computer and phone Library"));
    m_pdfLibraryAction->setVisible(true);
    m_pdfLibraryAction->setEnabled(true);
    connect(m_pdfLibraryAction, &QAction::triggered, this, [this] {
        if (!m_pendingSourceTarget.isEmpty()) {
            openPendingSourcePublicationOutput();
            return;
        }
        switch (activePdfLibraryState()) {
        case PdfLibraryState::SourceLinked:
            renderLinkedPdf();
            break;
        case PdfLibraryState::UnlinkedLibrary:
            removePdfFromLibrary();
            break;
        case PdfLibraryState::OutsideLibrary:
            if (!Thoughts::PdfVersions::isCachedVersion(activePdfUrl().toLocalFile())) trackPdf();
            break;
        case PdfLibraryState::NoDocument:
            choosePdfForLibrary();
            break;
        case PdfLibraryState::SourceLinkedMissingLayout:
        case PdfLibraryState::UnknownLibrary:
            break;
        }
    });
    m_pdfLibraryWidgetAction = actionCollection()->addAction(QStringLiteral("pdf_library_status"));
    m_pdfLibraryWidgetAction->setText(i18n("Sync"));
    m_pdfLibraryWidgetAction->setIconText(i18n("Sync"));
    m_pdfLibraryWidgetAction->setIcon(QIcon::fromTheme(QStringLiteral("view-refresh")));
    m_pdfLibraryWidgetAction->setToolTip(i18n("Sync with the server"));
    actionCollection()->setDefaultShortcut(m_pdfLibraryWidgetAction, QKeySequence(Qt::CTRL | Qt::Key_S));
    connect(m_pdfLibraryWidgetAction, &QAction::triggered, this, [this] { triggerFedoraSyncWithInitiationControl(QStringLiteral("pdf_toolbar_sync")); });
    // File > Copy for Agent: the open document's Copy for agent, without the tab menu (the tab bar
    // is hidden with one document).
    auto *copyForAgentAction = actionCollection()->addAction(QStringLiteral("copy_for_agent"));
    copyForAgentAction->setText(i18n("Copy for Agent"));
    copyForAgentAction->setIcon(QIcon::fromTheme(QStringLiteral("edit-copy")));
    actionCollection()->setDefaultShortcut(copyForAgentAction, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_C));
    connect(copyForAgentAction, &QAction::triggered, this, [this] {
        if (m_tabWidget) copyTabForAgent(m_tabWidget->currentIndex());
    });
    auto *syncDetailsAction = actionCollection()->addAction(QStringLiteral("pdf_sync_details"));
    syncDetailsAction->setText(i18n("Sync details…"));
    syncDetailsAction->setIcon(QIcon::fromTheme(QStringLiteral("dialog-information")));
    connect(syncDetailsAction, &QAction::triggered, this, &Shell::showPdfSyncMenu);
    m_pdfBuildStatusText = i18n("Sync");
    m_pdfBuildStatusAccessibleName = i18n("Sync with the server");
    m_pdfLibraryMenu = new QMenu(this);
    m_pdfLibraryMenu->setObjectName(QStringLiteral("pdfSyncDetailsMenu"));
    m_pdfSyncSummaryMenuAction = m_pdfLibraryMenu->addAction(i18n("Synchronization"));
    m_pdfSyncSummaryMenuAction->setEnabled(false);
    m_pdfSyncDetailMenuAction = m_pdfLibraryMenu->addAction(i18n("No previous sync"));
    m_pdfSyncDetailMenuAction->setEnabled(false);
    m_pdfSyncNowMenuAction = m_pdfLibraryMenu->addAction(QIcon::fromTheme(QStringLiteral("view-refresh")), i18n("Sync now"));
    connect(m_pdfSyncNowMenuAction, &QAction::triggered, this, [this] { triggerFedoraSyncWithInitiationControl(QStringLiteral("pdf_toolbar_sync")); });
    m_pdfLibraryMenu->addSeparator();
    m_pdfLibraryMenu->addSection(i18n("This PDF"));
    m_pdfStatusMenuAction = m_pdfLibraryMenu->addAction(QIcon::fromTheme(QStringLiteral("dialog-information")), i18n("Checking…"));
    m_pdfStatusMenuAction->setEnabled(false);
    m_pdfStatusDetailMenuAction = m_pdfLibraryMenu->addAction(i18n("Reading this PDF's status…"));
    m_pdfStatusDetailMenuAction->setEnabled(false);
    m_pdfLibraryMenu->addSeparator();
    m_pdfPrimaryMenuAction = m_pdfLibraryMenu->addAction(m_pdfLibraryAction->icon(), m_pdfLibraryAction->iconText());
    connect(m_pdfPrimaryMenuAction, &QAction::triggered, this, [this] {
        if (m_pdfPrimaryMenuCommand == QLatin1String("sync")) {
            triggerFedoraSyncWithInitiationControl(QStringLiteral("pdf_status_menu_sync"));
            return;
        }
        m_pdfLibraryAction->trigger();
    });
    m_pdfSourceConflictAction = m_pdfLibraryMenu->addAction(QIcon::fromTheme(QStringLiteral("vcs-conflicting")), i18n("Resolve source conflict…"));
    m_pdfSourceConflictAction->setVisible(false);
    m_pdfSourceConflictAction->setToolTip(i18n("Choose between this computer's and the server's TeX versions or edit a Git-style conflict file in Visual Studio Code."));
    connect(m_pdfSourceConflictAction, &QAction::triggered, this, &Shell::beginSourceConflictResolution);
    m_pdfLastBuiltMenuAction = m_pdfLibraryMenu->addAction(QIcon::fromTheme(QStringLiteral("view-history")), i18n("Logs…"));
    connect(m_pdfLastBuiltMenuAction, &QAction::triggered, this, &Shell::showSyncOperations);
    connect(m_pdfLibraryMenu, &QMenu::aboutToShow, this, &Shell::updatePdfLibraryAction);
    QAction *reviewAction = actionCollection()->addAction(QStringLiteral("review_overview"));
    reviewAction->setText(i18n("Review"));
    reviewAction->setIconText(i18n("Review"));
    reviewAction->setIcon(QIcon(new ReviewToolbarIconEngine));
    reviewAction->setToolTip(i18n("Open your review cards and start reviewing"));
    actionCollection()->setDefaultShortcut(reviewAction, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
    connect(reviewAction, &QAction::triggered, this, &Shell::showReviewOverview);
    QAction *agentNotesAction = actionCollection()->addAction(QStringLiteral("agent_notes"));
    agentNotesAction->setText(i18n("Agent Notes"));
    agentNotesAction->setIconText(i18n("Agent Notes"));
    actionCollection()->setDefaultShortcut(agentNotesAction, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N));
    connect(agentNotesAction, &QAction::triggered, this, [] { Shell::showAgentNotes(); });

    auto *windowButtonsSpace = new QWidget;
    windowButtonsSpace->setFixedSize(68, 1);
    windowButtonsSpace->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *windowButtonsSpaceAction = new QWidgetAction(this);
    windowButtonsSpaceAction->setDefaultWidget(windowButtonsSpace);
    windowButtonsSpaceAction->setText(i18n("Window buttons"));
    actionCollection()->addAction(QStringLiteral("window_buttons_space"), windowButtonsSpaceAction);

    // Clicks fall through to the bar, which moves the window.
    m_documentTitle = new QLabel;
    m_documentTitle->setObjectName(QStringLiteral("readerDocumentTitle"));
    m_documentTitle->setTextFormat(Qt::PlainText);
    m_documentTitle->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_documentTitle->setContentsMargins(8, 0, 0, 0);
    QFont documentTitleFont = m_documentTitle->font();
    documentTitleFont.setWeight(QFont::DemiBold);
    m_documentTitle->setFont(documentTitleFont);
    auto *documentTitleAction = new QWidgetAction(this);
    documentTitleAction->setDefaultWidget(m_documentTitle);
    documentTitleAction->setText(i18n("Document title"));
    actionCollection()->addAction(QStringLiteral("document_title"), documentTitleAction);
    connect(this, &QWidget::windowTitleChanged, this, &Shell::updateDocumentTitle);
    auto *serverAction = actionCollection()->addAction(QStringLiteral("thoughts_storage"));
    serverAction->setText(i18n("Server…"));
    serverAction->setIcon(QIcon::fromTheme(QStringLiteral("network-server")));
    auto *logsAction = actionCollection()->addAction(QStringLiteral("thoughts_logs"));
    logsAction->setText(i18n("Logs…"));
    logsAction->setIcon(QIcon::fromTheme(QStringLiteral("view-history")));
#ifdef Q_OS_MACOS
    serverAction->setMenuRole(QAction::ApplicationSpecificRole);
    logsAction->setMenuRole(QAction::ApplicationSpecificRole);
#endif
    connect(serverAction, &QAction::triggered, this, [this] {
        auto *overview = commentsOverview();
        if (KParts::ReadWritePart *part = fedoraGatewayPart()) {
            QVariantMap state;
            QMetaObject::invokeMethod(part, "fedoraRenderState", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, state));
            if (!state.isEmpty()) { overview->setPdfSyncState(QJsonObject::fromVariantMap(state)); }
        }
        overview->showServerDetails();
    });
    connect(logsAction, &QAction::triggered, this, &Shell::showSyncOperations);
    auto *developerLogsAction = actionCollection()->addAction(QStringLiteral("thoughts_developer_logs"));
    developerLogsAction->setText(i18n("Developer logs…"));
    developerLogsAction->setIcon(QIcon::fromTheme(QStringLiteral("utilities-terminal")));
#ifdef Q_OS_MACOS
    developerLogsAction->setMenuRole(QAction::ApplicationSpecificRole);
#endif
    connect(developerLogsAction, &QAction::triggered, this, [this] {
        Thoughts::showDeveloperLogs(this, [this] {
            QVariantMap diagnostics;
            if (KParts::ReadWritePart *part = fedoraGatewayPart()) {
                QMetaObject::invokeMethod(part, "sourceTrackingDiagnostics", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, diagnostics));
            }
            return diagnostics;
        });
    });
    if (Thoughts::Updates::available()) {
        auto *updatesAction = actionCollection()->addAction(QStringLiteral("thoughts_check_updates"));
        updatesAction->setText(i18n("Check for Updates…"));
        updatesAction->setIcon(QIcon::fromTheme(QStringLiteral("system-software-update")));
        connect(updatesAction, &QAction::triggered, this, [this] { Thoughts::Updates::checkInteractively(this); });
        static bool backgroundCheckStarted = false;
        if (!backgroundCheckStarted) {
            backgroundCheckStarted = true;
            Thoughts::Updates::checkInBackground(this);
        }
    }
    m_recent = KStandardAction::openRecent(this, SLOT(openUrl(QUrl)), actionCollection());
    m_recent->setToolBarMode(KRecentFilesAction::MenuMode);
    connect(m_recent, &QAction::triggered, this, &Shell::showOpenRecentMenu);
    connect(m_recent, &KRecentFilesAction::recentListCleared, this, &Shell::refreshRecentsOnWelcomeScreen);
    connect(m_welcomeScreen, &WelcomeScreen::forgetAllRecents, m_recent, &KRecentFilesAction::clear);
    m_recent->setToolTip(i18n("Click to open a file\nClick and hold to open a recent file"));
    m_recent->setWhatsThis(i18n("<b>Click</b> to open a file or <b>Click and hold</b> to select a recent file"));
    m_printAction = KStandardAction::print(this, SLOT(print()), actionCollection());
    m_printAction->setEnabled(false);
    m_closeAction = KStandardAction::close(this, SLOT(closeUrl()), actionCollection());
    m_closeAction->setEnabled(false);
    KStandardAction::quit(this, SLOT(close()), actionCollection());

    setStandardToolBarMenuEnabled(true);

    m_showMenuBarAction = KStandardAction::showMenubar(this, SLOT(slotShowMenubar()), actionCollection());
    m_fullScreenAction = KStandardAction::fullScreen(this, SLOT(slotUpdateFullScreen()), this, actionCollection());

    m_nextTabAction = actionCollection()->addAction(QStringLiteral("tab-next"));
    m_nextTabAction->setText(i18n("Next Tab"));
    actionCollection()->setDefaultShortcuts(m_nextTabAction, KStandardShortcut::tabNext());
    m_nextTabAction->setEnabled(false);
    connect(m_nextTabAction, &QAction::triggered, this, &Shell::activateNextTab);

    m_prevTabAction = actionCollection()->addAction(QStringLiteral("tab-previous"));
    m_prevTabAction->setText(i18n("Previous Tab"));
    actionCollection()->setDefaultShortcuts(m_prevTabAction, KStandardShortcut::tabPrev());
    m_prevTabAction->setEnabled(false);
    connect(m_prevTabAction, &QAction::triggered, this, &Shell::activatePrevTab);

    // add shortcuts for Shift+Alt+1 to Shift+Alt+9 to switch tabs(browser logic- Shift+Alt+9 is always going to be last tab)
    for (int i = 1; i <= 9; ++i) {
        QAction *action = actionCollection()->addAction(QStringLiteral("tab-switch-%1").arg(i));
        action->setText(i18n("Switch to Tab %1", i));

        // static cast to Qt::Key satisfies Qt 6 strict type checking for QKeySequence
        actionCollection()->setDefaultShortcut(action, QKeySequence(Qt::SHIFT | Qt::ALT | static_cast<Qt::Key>(Qt::Key_0 + i)));
        connect(action, &QAction::triggered, this, [this, i]() {
            if (m_tabs.isEmpty()) {
                return;
            }
            int index = (i == 9) ? m_tabs.size() - 1 : i - 1;
            if (index >= 0 && index < m_tabs.size()) {
                setActiveTab(index);
            }
        });
    }

    m_undoCloseTab = actionCollection()->addAction(QStringLiteral("undo-close-tab"));
    m_undoCloseTab->setText(i18n("Undo close tab"));
    actionCollection()->setDefaultShortcut(m_undoCloseTab, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T));
    m_undoCloseTab->setIcon(QIcon::fromTheme(QStringLiteral("edit-undo")));
    m_undoCloseTab->setEnabled(false);
    connect(m_undoCloseTab, &QAction::triggered, this, &Shell::undoCloseTab);

    m_lockSidebarAction = actionCollection()->addAction(QStringLiteral("okular_lock_sidebar"));
    m_lockSidebarAction->setCheckable(true);
    m_lockSidebarAction->setIcon(QIcon::fromTheme(QStringLiteral("lock")));
    m_lockSidebarAction->setText(i18n("Lock Sidebar"));
    connect(m_lockSidebarAction, &QAction::triggered, m_sidebar, &Sidebar::setLocked);
    m_sidebar->addAction(m_lockSidebarAction);
}

void Shell::showCommentsOverview()
{
    showCommentsOverview(true);
}

QString Shell::linkedPdfRenderTarget(const QString &docsRoot, const QUrl &url)
{
    if (!url.isLocalFile()) {
        return {};
    }

    const QDir currentPdfs(QDir(docsRoot).filePath(QStringLiteral("current_pdfs")));
    const QFileInfo currentPdfDirectory(currentPdfs.absolutePath());
    const QFileInfo pdf(url.toLocalFile());
    if (!currentPdfDirectory.exists() || !pdf.isFile() || pdf.suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) != 0 || pdf.canonicalPath() != currentPdfDirectory.canonicalFilePath()) {
        return {};
    }

    const QString stem = pdf.completeBaseName();
    if (stem.isEmpty()) {
        return {};
    }
    QFile sourceIndex(QDir(docsRoot).filePath(QStringLiteral("current_pdfs/.thoughts-source-index.json")));
    if (!sourceIndex.open(QIODevice::ReadOnly)) {
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument index = QJsonDocument::fromJson(sourceIndex.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !index.isObject() || index.object().value(QStringLiteral("schema_version")).toInt() != 1 || !index.object().value(QStringLiteral("pdfs")).isArray()) {
        return {};
    }
    static const QRegularExpression sha256Pattern(QStringLiteral("^[0-9a-f]{64}$"));
    bool found = false;
    for (const QJsonValue &value : index.object().value(QStringLiteral("pdfs")).toArray()) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("filename")).toString() != pdf.fileName()) {
            continue;
        }
        if (found || entry.value(QStringLiteral("project")).toString().trimmed().isEmpty() || !sha256Pattern.match(entry.value(QStringLiteral("pdf_sha256")).toString()).hasMatch()) {
            return {};
        }
        found = true;
    }
    return found ? QStringLiteral("current_pdfs/%1.pdf").arg(stem) : QString();
}

bool Shell::isExternalPdfForLibrary(const QString &docsRoot, const QUrl &url)
{
    if (!url.isLocalFile()) {
        return false;
    }
    const QFileInfo pdf(url.toLocalFile());
    if (!pdf.isFile() || !pdf.isReadable() || pdf.suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) != 0) {
        return false;
    }
    return !isCurrentLibraryPdf(docsRoot, pdf);
}

Shell::PdfLibraryState Shell::pdfLibraryState(const QString &docsRoot, const QUrl &url)
{
    if (!url.isLocalFile()) {
        return PdfLibraryState::NoDocument;
    }
    const QFileInfo pdf(url.toLocalFile());
    if (!pdf.isFile() || !pdf.isReadable() || pdf.suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) != 0) {
        return PdfLibraryState::NoDocument;
    }
    if (!isCurrentLibraryPdf(docsRoot, pdf)) {
        return PdfLibraryState::OutsideLibrary;
    }
    const LocalLibraryEntry entry = localLibraryEntry(docsRoot, pdf.fileName());
    if (!entry.available || !entry.found || !entry.generationCurrent) {
        return PdfLibraryState::UnknownLibrary;
    }
    if (entry.kind == QLatin1String("unlinked")) {
        return PdfLibraryState::UnlinkedLibrary;
    }
    return linkedPdfRenderTarget(docsRoot, url).isEmpty() ? PdfLibraryState::SourceLinkedMissingLayout : PdfLibraryState::SourceLinked;
}

QUrl Shell::activePdfUrl() const
{
    const int activeTab = m_tabWidget ? m_tabWidget->currentIndex() : -1;
    return activeTab >= 0 && activeTab < m_tabs.size() ? m_tabs.at(activeTab).part->url() : QUrl();
}

Shell::PdfLibraryState Shell::quickActivePdfLibraryState() const
{
    const QUrl url = activePdfUrl();
    if (url.isEmpty()) {
        return PdfLibraryState::NoDocument;
    }
    if (!url.isLocalFile()) {
        return PdfLibraryState::OutsideLibrary;
    }
    const QFileInfo pdf(url.toLocalFile());
    if (pdf.suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) != 0) {
        return PdfLibraryState::NoDocument;
    }
    const QString currentPdfsPath = QDir::cleanPath(QDir(ShellUtils::docsRoot()).filePath(QStringLiteral("current_pdfs")));
    const QString documentDirectory = QDir::cleanPath(pdf.absolutePath());
    const QFileInfo directoryInfo(documentDirectory);
    const bool generationTarget = QDir::cleanPath(directoryInfo.absolutePath()) == QDir::cleanPath(ShellUtils::docsRoot()) && directoryInfo.fileName().startsWith(QLatin1String(".thoughts-library-stage-"));
    return documentDirectory == currentPdfsPath || generationTarget ? PdfLibraryState::UnknownLibrary : PdfLibraryState::OutsideLibrary;
}

void Shell::requestPdfLibraryStateRefresh(bool force)
{
    const QUrl url = activePdfUrl();
    if (m_pdfLibraryRefreshRunning) {
        // Routine presentation updates can arrive while this worker is still
        // reading the same PDF state. The in-flight result already satisfies
        // those updates; only a real invalidation or URL change needs another
        // read after it finishes.
        if (force || url != m_pdfLibraryActiveRefreshUrl) {
            m_pdfLibraryRefreshQueued = true;
        }
        return;
    }
    if (!force && m_pdfLibraryStateReady && url == m_pdfLibraryStateUrl) {
        return;
    }
    if (url != m_pdfLibraryStateUrl) {
        m_pdfLibraryStateReady = false;
    }

    const QString docsRoot = ShellUtils::docsRoot();
    const quint64 requestSerial = ++m_pdfLibraryRefreshSerial;
    m_pdfLibraryActiveRefreshSerial = requestSerial;
    m_pdfLibraryActiveRefreshUrl = url;
    m_pdfLibraryRefreshRunning = true;
    const QPointer<Shell> shell(this);
    QThreadPool::globalInstance()->start([shell, docsRoot, url, requestSerial] {
        QVariantMap snapshot;
        snapshot.insert(QStringLiteral("url"), url);
        snapshot.insert(QStringLiteral("request_serial"), requestSerial);
        PdfLibraryState state = PdfLibraryState::NoDocument;
        LocalLibraryEntry entry;
        QString renderTarget;
        if (url.isLocalFile()) {
            const QFileInfo pdf(url.toLocalFile());
            if (pdf.isFile() && pdf.isReadable() && pdf.suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0) {
                if (!isCurrentLibraryPdf(docsRoot, pdf)) {
                    state = PdfLibraryState::OutsideLibrary;
                } else {
                    entry = localLibraryEntry(docsRoot, pdf.fileName());
                    if (!entry.available || !entry.found || !entry.generationCurrent) {
                        state = PdfLibraryState::UnknownLibrary;
                    } else if (entry.kind == QLatin1String("unlinked")) {
                        state = PdfLibraryState::UnlinkedLibrary;
                    } else {
                        renderTarget = linkedPdfRenderTarget(docsRoot, url);
                        state = renderTarget.isEmpty() ? PdfLibraryState::SourceLinkedMissingLayout : PdfLibraryState::SourceLinked;
                    }
                }
            }
        }
        snapshot.insert(QStringLiteral("state"), static_cast<int>(state));
        snapshot.insert(QStringLiteral("render_target"), renderTarget);
        snapshot.insert(QStringLiteral("sha256"), entry.sha256);
        snapshot.insert(QStringLiteral("display_name"), entry.displayName);
        snapshot.insert(QStringLiteral("published_at"), entry.publishedAt);
        snapshot.insert(QStringLiteral("generation_current"), entry.generationCurrent);
        snapshot.insert(QStringLiteral("pdf_stale"), entry.pdfStale);
        snapshot.insert(QStringLiteral("latest_build_status"), entry.latestBuildStatus);
        snapshot.insert(QStringLiteral("latest_build_message"), entry.latestBuildMessage);
        snapshot.insert(QStringLiteral("last_build_id"), entry.lastCompletedBuildId);
        snapshot.insert(QStringLiteral("local_freshness_available"), entry.localFreshnessAvailable);
        snapshot.insert(QStringLiteral("local_sources_current"), entry.localSourcesCurrent);
        snapshot.insert(QStringLiteral("local_freshness_message"), entry.localFreshnessMessage);
        snapshot.insert(QStringLiteral("local_freshness_watch_files"), entry.localFreshnessWatchFiles);
        snapshot.insert(QStringLiteral("local_freshness_watch_directories"), entry.localFreshnessWatchDirectories);
        if (!shell) {
            return;
        }
        QMetaObject::invokeMethod(
            shell,
            [shell, snapshot] {
                if (shell) {
                    shell->finishPdfLibraryStateRefresh(snapshot);
                }
            },
            Qt::QueuedConnection);
    });
}

void Shell::finishPdfLibraryStateRefresh(const QVariantMap &snapshot)
{
    m_pdfLibraryRefreshRunning = false;
    const bool refreshAgain = std::exchange(m_pdfLibraryRefreshQueued, false);
    const quint64 requestSerial = snapshot.value(QStringLiteral("request_serial")).toULongLong();
    const QUrl snapshotUrl = snapshot.value(QStringLiteral("url")).toUrl();
    const QUrl currentUrl = activePdfUrl();
    if (requestSerial != m_pdfLibraryActiveRefreshSerial || refreshAgain || snapshotUrl != currentUrl) {
        // The active tab, its URL, or a watched source changed while this
        // snapshot was being read. Never let an obsolete empty-URL result turn
        // a successfully reopened document into "No PDF".
        m_pdfLibraryStateReady = false;
        updatePdfLibraryAction();
        return;
    }

    m_pdfLibraryActiveRefreshSerial = 0;
    m_pdfLibraryActiveRefreshUrl = {};
    m_pdfLibraryStateUrl = snapshotUrl;
    const int stateValue = snapshot.contains(QStringLiteral("state")) ? snapshot.value(QStringLiteral("state")).toInt() : static_cast<int>(PdfLibraryState::UnknownLibrary);
    m_cachedPdfLibraryState = static_cast<PdfLibraryState>(stateValue);
    m_cachedPdfLibraryRenderTarget = snapshot.value(QStringLiteral("render_target")).toString();
    m_cachedPdfLibrarySha256 = snapshot.value(QStringLiteral("sha256")).toString();
    m_cachedPdfLibraryDisplayName = snapshot.value(QStringLiteral("display_name")).toString();
    m_cachedPdfLibraryPublishedAt = snapshot.value(QStringLiteral("published_at")).toDateTime();
    m_cachedPdfLibraryGenerationCurrent = snapshot.value(QStringLiteral("generation_current")).toBool();
    m_cachedPdfLibraryStale = snapshot.value(QStringLiteral("pdf_stale")).toBool();
    m_cachedPdfLibraryBuildStatus = snapshot.value(QStringLiteral("latest_build_status")).toString();
    m_cachedPdfLibraryBuildMessage = snapshot.value(QStringLiteral("latest_build_message")).toString();
    m_cachedPdfLibraryLastBuildId = snapshot.value(QStringLiteral("last_build_id")).toString();
    m_cachedPdfLibraryLocalFreshnessAvailable = snapshot.value(QStringLiteral("local_freshness_available")).toBool();
    m_cachedPdfLibraryLocalSourcesCurrent = snapshot.value(QStringLiteral("local_sources_current")).toBool();
    m_cachedPdfLibraryLocalFreshnessMessage = snapshot.value(QStringLiteral("local_freshness_message")).toString();
    configurePdfSourceFreshnessWatcher(snapshot.value(QStringLiteral("local_freshness_watch_files")).toStringList(), snapshot.value(QStringLiteral("local_freshness_watch_directories")).toStringList());
    m_pdfLibraryStateReady = true;
    updatePdfLibraryAction();
}

void Shell::schedulePdfLibraryPartRefresh(const KParts::ReadWritePart *part)
{
    const int activeTab = m_tabWidget ? m_tabWidget->currentIndex() : -1;
    if (!part || activeTab < 0 || activeTab >= m_tabs.size() || m_tabs.at(activeTab).part != part || m_pdfLibraryPartRefreshScheduled) {
        return;
    }
    m_pdfLibraryPartRefreshScheduled = true;
    QTimer::singleShot(0, this, [this] {
        m_pdfLibraryPartRefreshScheduled = false;
        m_pdfLibraryStateReady = false;
        requestPdfLibraryStateRefresh(true);
        updatePdfLibraryAction();
    });
}

void Shell::configurePdfSourceFreshnessWatcher(const QStringList &files, const QStringList &directories)
{
    // Missing files are represented by their directory; any entry reported
    // inside such a directory may be the file appearing.
    m_pdfSourceFreshnessPaths.clear();
    m_pdfSourceFreshnessDirectories.clear();
    const auto add = [](QSet<QString> &set, const QString &path) {
        const QFileInfo info(path);
        set.insert(QDir::cleanPath(info.absoluteFilePath()));
        const QString canonical = info.canonicalFilePath();
        if (!canonical.isEmpty()) {
            set.insert(canonical);
        }
    };
    for (const QString &path : files) {
        add(m_pdfSourceFreshnessPaths, path);
    }
    for (const QString &path : directories) {
        add(m_pdfSourceFreshnessPaths, path);
        add(m_pdfSourceFreshnessDirectories, path);
    }
}

void Shell::handlePdfSourcePathsChanged(const QStringList &paths)
{
    if (m_pdfSourceFreshnessPaths.isEmpty()) {
        return;
    }
    for (const QString &path : paths) {
        if (m_pdfSourceFreshnessPaths.contains(path) || m_pdfSourceFreshnessDirectories.contains(QFileInfo(path).absolutePath())) {
            schedulePdfSourceFreshnessRefresh();
            return;
        }
    }
}

void Shell::schedulePdfSourceFreshnessRefresh()
{
    if (m_pdfSourceFreshnessTimer) {
        m_pdfSourceFreshnessTimer->start();
    }
}

QVariantMap Shell::localSourceFreshnessForTesting(const QString &docsRoot,
                                                  const QString &project,
                                                  const QString &sourceManifestPath,
                                                  const QString &publicationStatePath,
                                                  const QString &target,
                                                  const QString &expectedInputSnapshotSha256,
                                                  const QVariantMap &buildInputInspection)
{
    const std::optional<QJsonObject> providedInspection = buildInputInspection.isEmpty() ? std::nullopt : std::optional<QJsonObject>(QJsonObject::fromVariantMap(buildInputInspection));
    const LocalSourceFreshness freshness = evaluateLocalSourceFreshness(docsRoot, project, sourceManifestPath, publicationStatePath, target, expectedInputSnapshotSha256, providedInspection);
    return {
        {QStringLiteral("available"), freshness.available},
        {QStringLiteral("current"), freshness.current},
        {QStringLiteral("message"), freshness.message},
        {QStringLiteral("watch_files"), freshness.watchFiles},
        {QStringLiteral("watch_directories"), freshness.watchDirectories},
    };
}

Shell::PdfLibraryState Shell::activePdfLibraryState() const
{
    const QUrl url = activePdfUrl();
    if (m_pdfLibraryStateReady && url == m_pdfLibraryStateUrl) {
        // KParts keeps the original HTTP(S) URL on a downloaded document, so the URL-only worker cannot recognize the readable local PDF backing that is already open in the part.
        if (m_cachedPdfLibraryState == PdfLibraryState::NoDocument && !url.isLocalFile() && !activePdfSourcePath().isEmpty()) {
            return PdfLibraryState::OutsideLibrary;
        }
        return m_cachedPdfLibraryState;
    }
    return quickActivePdfLibraryState();
}

QVariantMap Shell::activeFedoraBuildActivity() const
{
    const int activeTab = m_tabWidget ? m_tabWidget->currentIndex() : -1;
    if (activeTab < 0 || activeTab >= m_tabs.size()) {
        return {};
    }
    const QUrl url = activePdfUrl();
    if (!url.isLocalFile()) {
        return {};
    }
    QVariantMap activity;
    QMetaObject::invokeMethod(m_tabs.at(activeTab).part, "fedoraBuildActivity", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, activity), Q_ARG(QString, QFileInfo(url.toLocalFile()).fileName()));
    return activity;
}

QVariantMap Shell::activeFedoraRenderStatus()
{
    const int activeTab = m_tabWidget ? m_tabWidget->currentIndex() : -1;
    if (activeTab < 0 || activeTab >= m_tabs.size()) {
        return {};
    }
    const QUrl url = activePdfUrl();
    if (!url.isLocalFile()) {
        return {};
    }
    const QString target = QFileInfo(url.toLocalFile()).fileName();
    QVariantMap status;
    QMetaObject::invokeMethod(m_tabs.at(activeTab).part, "fedoraRenderStatus", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, status), Q_ARG(QString, target));
    if (!status.isEmpty()) {
        m_cachedFedoraRenderTarget = target;
        m_cachedFedoraRenderStatus = status;
    }
    return m_cachedFedoraRenderTarget == target ? m_cachedFedoraRenderStatus : QVariantMap();
}

void Shell::handleFedoraRenderStatusChanged(const QString &target, const QJsonObject &status)
{
    scheduleCommentsOverviewDocumentSyncPresentationUpdate();
    if (target == m_pendingSourceTarget) {
        m_cachedFedoraRenderTarget = target;
        m_cachedFedoraRenderStatus = status.toVariantMap();
        const QString phase = status.value(QStringLiteral("status")).toString();
        const QString buildId = status.value(QStringLiteral("build")).toObject().value(QStringLiteral("id")).toString();
        const QString progressEvent = Thoughts::fedoraBuildProgressEvent(status);
        if (!m_activeBuildOperationId.isEmpty() && target == m_activeBuildOperationTarget) {
            if (phase == QLatin1String("building")) {
                updateSyncOperation(m_activeBuildOperationId, Thoughts::SyncOperationState::Running, QStringLiteral("building"), progressEvent.isEmpty() ? Thoughts::SyncStatusLabels::syncing() : progressEvent, buildId);
            } else if (phase == QLatin1String("publishing") && status.value(QStringLiteral("queued")).toBool()) {
                updateSyncOperation(m_activeBuildOperationId, Thoughts::SyncOperationState::Running, QStringLiteral("waiting_source_publication"), i18n("Waiting to publish sources…"));
            } else if (phase == QLatin1String("preparing")) {
                updateSyncOperation(m_activeBuildOperationId, Thoughts::SyncOperationState::Running, QStringLiteral("preflight"), i18n("Preparing source publication…"), buildId);
            } else if (phase == QLatin1String("publishing")) {
                updateSyncOperation(m_activeBuildOperationId, Thoughts::SyncOperationState::Running, QStringLiteral("uploading"), i18n("Publishing sources…"), buildId);
            } else if (phase == QLatin1String("activating")) {
                updateSyncOperation(m_activeBuildOperationId, Thoughts::SyncOperationState::Running, QStringLiteral("activating"), i18n("Installing the updated PDF…"), buildId);
            }
        }
        if (phase == QLatin1String("error")) {
            const QString reason = status.value(QStringLiteral("reason")).toString().trimmed();
            const QString errorCode = status.value(QStringLiteral("error_code")).toString(status.value(QStringLiteral("build")).toObject().value(QStringLiteral("error_code")).toString(QStringLiteral("build_failed")));
            updateSyncOperation(m_activeBuildOperationId, Thoughts::SyncOperationState::Failed, QStringLiteral("failed"), reason.isEmpty() ? i18n("Build failed") : reason, buildId, errorCode);
            m_activeBuildOperationId.clear();
            m_activeBuildOperationTarget.clear();
            m_pendingSourceProject.clear();
            m_pendingSourceTarget.clear();
            updatePdfLibraryAction();
            KMessageBox::error(this, reason.isEmpty() ? i18n("The source document could not be added to the Library.") : reason, i18n("Publish Source Document"));
            return;
        }
        if (phase == QLatin1String("synced")) {
            updateSyncOperation(m_activeBuildOperationId, Thoughts::SyncOperationState::Succeeded, QStringLiteral("succeeded"), i18n("Completed"), buildId);
            m_activeBuildOperationId.clear();
            m_activeBuildOperationTarget.clear();
            m_pendingSourceProject.clear();
            m_pendingSourceTarget.clear();
            m_pdfLibraryStateReady = false;
            if (s_commentsOverview) {
                s_commentsOverview->requestReload();
            }
        }
        updatePdfLibraryAction();
        return;
    }
    const bool refreshed = status.value(QStringLiteral("refreshed")).toBool();
    const QString remoteState = status.value(QStringLiteral("status")).toString();
    const QString localState = status.value(QStringLiteral("local_state")).toString();
    const bool rebuildNeeded = remoteState == QLatin1String("out_of_sync") || remoteState == QLatin1String("attention") || remoteState == QLatin1String("error");
    if (m_pendingRebuildAfterRefreshTarget == target && !status.value(QStringLiteral("checking")).toBool()) {
        const QString initiationControl = std::exchange(m_pendingRebuildInitiationControl, QString());
        m_pendingRebuildAfterRefreshTarget.clear();
        m_rebuildAuthorizedTarget.clear();
        KParts::ReadWritePart *part = fedoraGatewayPart();
        QVariantMap pendingDocumentSync;
        if (part) {
            QMetaObject::invokeMethod(part, "documentSyncStatus", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, pendingDocumentSync), Q_ARG(QString, target));
        }
        const bool synchronizedInputs = m_pendingRebuildSourcesCurrent && pendingDocumentSync.value(QStringLiteral("total")).toInt() == 0;
        m_pendingRebuildSourcesCurrent = false;
        if (refreshed && rebuildNeeded && synchronizedInputs) {
            if (m_activeBuildOperationId.isEmpty()) {
                m_activeBuildOperationId = beginSyncOperation(QStringLiteral("build"), target, QStringLiteral("waiting_source_publication"), initiationControl);
                m_activeBuildOperationTarget = target;
            }
            if (!requestFedoraBuildForTarget(part, target, m_activeBuildOperationId)) {
                updateSyncOperation(m_activeBuildOperationId, Thoughts::SyncOperationState::Failed, QStringLiteral("failed"), i18n("Build could not be started"));
                m_activeBuildOperationId.clear();
                m_activeBuildOperationTarget.clear();
                KMessageBox::error(this, i18n("Sync with the server is unavailable for this document."), i18n("Rebuild PDF"));
            }
        }
    }
    if (QFileInfo(activePdfUrl().toLocalFile()).fileName() != target) {
        return;
    }
    const QString previousLocalState = m_cachedFedoraRenderTarget == target ? m_cachedFedoraRenderStatus.value(QStringLiteral("local_state")).toString() : QString();
    m_cachedFedoraRenderTarget = target;
    m_cachedFedoraRenderStatus = status.toVariantMap();
    if (status.value(QStringLiteral("checking")).toBool()) {
        updatePdfLibraryAction();
        return;
    }
    const QString operationId = (!m_activeBuildOperationId.isEmpty() && target == m_activeBuildOperationTarget) ? m_activeBuildOperationId : runningBuildOperationIdForTarget(target);
    if (!operationId.isEmpty()) {
        const QString buildId = status.value(QStringLiteral("build")).toObject().value(QStringLiteral("id")).toString();
        const QString progressEvent = Thoughts::fedoraBuildProgressEvent(status);
        if (remoteState == QLatin1String("building")) {
            updateSyncOperation(operationId, Thoughts::SyncOperationState::Running, QStringLiteral("building"), progressEvent.isEmpty() ? Thoughts::SyncStatusLabels::syncing() : progressEvent, buildId);
        } else if (remoteState == QLatin1String("publishing") && status.value(QStringLiteral("queued")).toBool()) {
            updateSyncOperation(operationId, Thoughts::SyncOperationState::Running, QStringLiteral("waiting_source_publication"), i18n("Waiting to publish sources…"));
        } else if (remoteState == QLatin1String("preparing")) {
            updateSyncOperation(operationId, Thoughts::SyncOperationState::Running, QStringLiteral("preflight"), i18n("Preparing source publication…"), buildId);
        } else if (remoteState == QLatin1String("publishing")) {
            updateSyncOperation(operationId, Thoughts::SyncOperationState::Running, QStringLiteral("uploading"), i18n("Publishing sources…"), buildId);
        } else if (remoteState == QLatin1String("synced") && localState == QLatin1String("sync_pending")) {
            updateSyncOperation(operationId, Thoughts::SyncOperationState::Running, QStringLiteral("activating"), i18n("Installing the updated PDF…"), buildId);
        } else if (remoteState == QLatin1String("synced") && localState == QLatin1String("current")) {
            updateSyncOperation(operationId, Thoughts::SyncOperationState::Succeeded, QStringLiteral("succeeded"), i18n("Completed"), buildId);
            if (operationId == m_activeBuildOperationId) {
                m_activeBuildOperationId.clear();
                m_activeBuildOperationTarget.clear();
            }
        } else if (remoteState == QLatin1String("error") || remoteState == QLatin1String("attention")) {
            const QString reason = status.value(QStringLiteral("reason")).toString().trimmed();
            const QString errorCode = status.value(QStringLiteral("error_code")).toString(status.value(QStringLiteral("build")).toObject().value(QStringLiteral("error_code")).toString(QStringLiteral("build_failed")));
            updateSyncOperation(operationId, Thoughts::SyncOperationState::Failed, QStringLiteral("failed"), reason.isEmpty() ? i18n("Build failed") : reason, buildId, errorCode);
            if (operationId == m_activeBuildOperationId) {
                m_activeBuildOperationId.clear();
                m_activeBuildOperationTarget.clear();
            }
        }
    }
    const bool localSyncNeeded = remoteState == QLatin1String("synced") && (localState == QLatin1String("sync_pending") || localState == QLatin1String("error"));
    if (localState == QLatin1String("current") && previousLocalState != localState) {
        m_pdfLibraryStateReady = false;
    }
    if (refreshed) {
        if (localSyncNeeded || (m_cachedPdfLibraryLocalFreshnessAvailable && !m_cachedPdfLibraryLocalSourcesCurrent)) {
            m_rebuildAuthorizedTarget.clear();
        } else if (rebuildNeeded) {
            m_rebuildAuthorizedTarget = target;
        } else if (m_rebuildAuthorizedTarget == target) {
            m_rebuildAuthorizedTarget.clear();
        }
    }
    updatePdfLibraryAction();
}

QString Shell::activePdfSourcePath() const
{
    const int activeTab = m_tabWidget ? m_tabWidget->currentIndex() : -1;
    if (activeTab < 0 || activeTab >= m_tabs.size()) {
        return {};
    }
    KParts::ReadWritePart *part = m_tabs.at(activeTab).part;
    QString path;
    QMetaObject::invokeMethod(part, "localDocumentPath", Qt::DirectConnection, Q_RETURN_ARG(QString, path));
    if (path.isEmpty() && part->url().isLocalFile()) {
        path = part->url().toLocalFile();
    }
    const QFileInfo file(path);
    return file.isFile() && file.isReadable() && readablePdfPayload(file.absoluteFilePath()) ? file.absoluteFilePath() : QString();
}

QString Shell::pdfLinkSafeFilename(const QString &value)
{
    QString base = QFileInfo(value.isEmpty() ? QStringLiteral("document.pdf") : value).fileName();
    if (base.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive)) {
        base.chop(4);
    }
    QString sanitized;
    sanitized.reserve(base.size());
    const auto asciiLetterOrDigit = [](QChar character) {
        const ushort value = character.unicode();
        return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9');
    };
    for (const QChar character : std::as_const(base)) {
        sanitized.append(asciiLetterOrDigit(character) || character == QLatin1Char('.') || character == QLatin1Char('_') || character == QLatin1Char('-') ? character : QLatin1Char('_'));
    }
    sanitized.replace(QRegularExpression(QStringLiteral("_+")), QStringLiteral("_"));
    if (sanitized.isEmpty()) {
        sanitized = QStringLiteral("document");
    } else if (!asciiLetterOrDigit(sanitized.at(0))) {
        qsizetype firstAlphaNumeric = 0;
        while (firstAlphaNumeric < sanitized.size() && !asciiLetterOrDigit(sanitized.at(firstAlphaNumeric))) {
            ++firstAlphaNumeric;
        }
        const QString suffix = firstAlphaNumeric < sanitized.size() ? sanitized.mid(firstAlphaNumeric) : QStringLiteral("pdf");
        sanitized = QStringLiteral("document_%1").arg(suffix);
    }
    return sanitized + QStringLiteral(".pdf");
}

QString Shell::activePdfSuggestedFilename() const
{
    const int activeTab = m_tabWidget ? m_tabWidget->currentIndex() : -1;
    if (activeTab < 0 || activeTab >= m_tabs.size()) {
        return QStringLiteral("document.pdf");
    }
    const QUrl url = activePdfUrl();
    const QString suggested = url.isLocalFile() ? QFileInfo(url.toLocalFile()).fileName() : url.fileName();
    return pdfLinkSafeFilename(suggested);
}

QString Shell::libraryPdfSha256(const QString &docsRoot, const QUrl &url)
{
    if (!url.isLocalFile()) {
        return {};
    }
    const QFileInfo pdf(url.toLocalFile());
    if (!pdf.isFile() || !isCurrentLibraryPdf(docsRoot, pdf)) {
        return {};
    }
    const LocalLibraryEntry entry = localLibraryEntry(docsRoot, pdf.fileName());
    return entry.available && entry.found ? entry.sha256 : QString();
}

void Shell::updatePdfLibraryWidget(bool sourceLinked, const QString &status, const QString &reason, Thoughts::DocumentSyncStatus syncState)
{
    if (!m_pdfLibraryWidgetAction || !m_pdfStatusMenuAction || !m_pdfStatusDetailMenuAction || !m_pdfPrimaryMenuAction || !m_pdfSourceConflictAction || !m_pdfLastBuiltMenuAction || !m_pdfLibraryAction) {
        return;
    }

    m_pdfSourceConflictAction->setVisible(syncOperationOffersSourceConflictResolution(latestGlobalSyncOperationSummary()));

    if (!sourceLinked) {
        const QString actionState = m_pdfLibraryAction->data().toString();
        QString shortStatus = status.isEmpty() ? i18n("Not in Library") : status;
        if (status.isEmpty() && actionState == QLatin1String("adding-library-pdf")) {
            shortStatus = i18n("Adding…");
        } else if (status.isEmpty() && actionState == QLatin1String("removing-library-pdf")) {
            shortStatus = i18n("Removing…");
        } else if (status.isEmpty() && actionState == QLatin1String("unlinked-library")) {
            shortStatus = i18n("In Library");
        } else if (status.isEmpty() && actionState == QLatin1String("no-document")) {
            shortStatus = i18n("No PDF");
        } else if (status.isEmpty() && actionState == QLatin1String("unknown-library")) {
            shortStatus = i18n("Unavailable");
        }
        m_pdfBuildStatusAccessibleName = i18n("Sync with the server. PDF status: %1", shortStatus);
        m_pdfStatusMenuAction->setText(shortStatus);
        m_pdfStatusMenuAction->setIcon(m_pdfLibraryAction->icon());
        m_pdfStatusMenuAction->setData(QStringLiteral("library_management"));
        m_pdfStatusMenuAction->setToolTip(reason.isEmpty() ? m_pdfLibraryAction->toolTip() : reason);
        const QString fullDetail = reason.isEmpty() ? m_pdfLibraryAction->toolTip() : reason;
        m_pdfStatusDetailMenuAction->setText(compactMenuDetail(fullDetail));
        m_pdfStatusDetailMenuAction->setToolTip(fullDetail);
        m_pdfStatusDetailMenuAction->setVisible(!m_pdfStatusDetailMenuAction->text().isEmpty());
        m_pdfPrimaryMenuCommand = QStringLiteral("manage");
        m_pdfPrimaryMenuAction->setText(m_pdfLibraryAction->iconText());
        m_pdfPrimaryMenuAction->setIcon(m_pdfLibraryAction->icon());
        m_pdfPrimaryMenuAction->setToolTip(m_pdfLibraryAction->toolTip());
        m_pdfPrimaryMenuAction->setEnabled(m_pdfLibraryAction->isEnabled());
        m_pdfPrimaryMenuAction->setVisible(true);
        m_pdfLastBuiltMenuAction->setText(i18n("Logs…"));
        m_pdfLastBuiltMenuAction->setVisible(true);
        m_pdfLastBuiltMenuAction->setEnabled(true);
        m_pdfLastBuiltMenuAction->setToolTip(i18n("Show recent sync and build operations and their logs."));
        updatePdfSyncStatus();
        return;
    }

    QString iconName = QStringLiteral("dialog-warning");
    if (syncState == Thoughts::DocumentSyncStatus::Current) {
        iconName = QStringLiteral("document-save");
    } else if (syncState == Thoughts::DocumentSyncStatus::SyncNeeded || syncState == Thoughts::DocumentSyncStatus::BuildNeeded) {
        iconName = QStringLiteral("view-refresh");
    } else if (syncState == Thoughts::DocumentSyncStatus::Building || syncState == Thoughts::DocumentSyncStatus::Syncing || syncState == Thoughts::DocumentSyncStatus::WaitingForSourcePublication) {
        iconName = QStringLiteral("system-run");
    }

    QString detail = reason;
    if (syncState == Thoughts::DocumentSyncStatus::Current) {
        QDateTime checkedAt = QDateTime::fromString(m_cachedFedoraRenderStatus.value(QStringLiteral("checked_at")).toString(), Qt::ISODateWithMs);
        if (!checkedAt.isValid()) {
            checkedAt = QDateTime::fromString(m_cachedFedoraRenderStatus.value(QStringLiteral("checked_at")).toString(), Qt::ISODate);
        }
        if (checkedAt.isValid()) {
            detail = i18n("Last verified synced: %1", QLocale().toString(checkedAt.toLocalTime(), QLocale::LongFormat));
        } else if (detail.isEmpty()) {
            detail = i18n("The local PDF matches the verified server publication.");
        }
    }

    m_pdfStatusMenuAction->setText(status);
    m_pdfStatusMenuAction->setIcon(QIcon::fromTheme(iconName));
    m_pdfStatusMenuAction->setData(syncState == Thoughts::DocumentSyncStatus::SyncNeeded        ? QStringLiteral("sync_needed")
                                       : syncState == Thoughts::DocumentSyncStatus::BuildNeeded ? QStringLiteral("build_needed")
                                       : syncState == Thoughts::DocumentSyncStatus::Current     ? QStringLiteral("synced")
                                       : syncState == Thoughts::DocumentSyncStatus::Error       ? QStringLiteral("error")
                                                                                                : QStringLiteral("progress"));
    m_pdfStatusMenuAction->setToolTip(detail);
    m_pdfStatusDetailMenuAction->setText(compactMenuDetail(detail));
    m_pdfStatusDetailMenuAction->setToolTip(detail);
    m_pdfStatusDetailMenuAction->setVisible(!detail.isEmpty());
    m_pdfBuildStatusAccessibleName = i18n("Sync with the server. PDF status: %1", status);

    const bool buildActive = m_pdfLibraryAction->data().toString() == QLatin1String("source-linked-build-active");
    m_pdfPrimaryMenuCommand = QStringLiteral("manage");
    QString primaryText;
    if (syncState == Thoughts::DocumentSyncStatus::SyncNeeded) {
        primaryText = i18n("Sync");
        m_pdfPrimaryMenuCommand = QStringLiteral("sync");
    } else if (syncState == Thoughts::DocumentSyncStatus::BuildNeeded || syncState == Thoughts::DocumentSyncStatus::Error) {
        primaryText = m_pdfLibraryAction->iconText();
        m_pdfPrimaryMenuCommand = primaryText == i18n("Sync") ? QStringLiteral("sync") : QStringLiteral("manage");
    } else if (buildActive) {
        primaryText = i18n("Open live build output");
    }
    m_pdfPrimaryMenuAction->setText(primaryText);
    m_pdfPrimaryMenuAction->setIcon(m_pdfLibraryAction->icon());
    m_pdfPrimaryMenuAction->setToolTip(m_pdfLibraryAction->toolTip());
    m_pdfPrimaryMenuAction->setEnabled(!primaryText.isEmpty() && (m_pdfPrimaryMenuCommand == QLatin1String("sync") || m_pdfLibraryAction->isEnabled()));
    m_pdfPrimaryMenuAction->setVisible(!primaryText.isEmpty());
    m_pdfLastBuiltMenuAction->setVisible(true);
    m_pdfLastBuiltMenuAction->setText(i18n("Logs…"));
    m_pdfLastBuiltMenuAction->setEnabled(true);
    m_pdfLastBuiltMenuAction->setToolTip(i18n("Show recent sync and build operations and their logs."));
    updatePdfSyncStatus();
}

void Shell::updatePdfLibraryAction()
{
    if (!m_pdfLibraryAction) {
        return;
    }
    requestPdfLibraryStateRefresh();
    m_pdfLibraryAction->setText(i18n("Manage Current PDF"));
    const QUrl currentPdfUrl = activePdfUrl();
    const QString interactionTarget = currentPdfUrl.isLocalFile() ? QFileInfo(currentPdfUrl.toLocalFile()).fileName() : QString();
    if (m_pdfStatusInteractionTarget.isEmpty()) {
        m_pdfStatusInteractionTarget = interactionTarget;
    } else if (interactionTarget != m_pdfStatusInteractionTarget) {
        m_pdfStatusInteractionTarget = interactionTarget;
        m_rebuildAuthorizedTarget.clear();
        m_pendingRebuildAfterRefreshTarget.clear();
    }
    if (!currentPdfUrl.isEmpty() && (!m_pdfLibraryStateReady || currentPdfUrl != m_pdfLibraryStateUrl)) {
        m_pdfLibraryAction->setIconText(Thoughts::SyncStatusLabels::syncing());
        m_pdfLibraryAction->setIcon(QIcon::fromTheme(QStringLiteral("dialog-information")));
        m_pdfLibraryAction->setToolTip(i18n("Reading the local PDF Library status"));
        m_pdfLibraryAction->setVisible(true);
        m_pdfLibraryAction->setEnabled(false);
        m_pdfLibraryAction->setData(QStringLiteral("checking-library-pdf"));
        updatePdfLibraryWidget(true, Thoughts::SyncStatusLabels::syncing(), i18n("Reading the local PDF Library status."), Thoughts::DocumentSyncStatus::Syncing);
        return;
    }
    const PdfLibraryState state = activePdfLibraryState();
    LocalLibraryEntry activeEntry;
    if (m_pdfLibraryStateReady && activePdfUrl() == m_pdfLibraryStateUrl) {
        activeEntry.available = true;
        activeEntry.found = !m_cachedPdfLibrarySha256.isEmpty();
        activeEntry.generationCurrent = m_cachedPdfLibraryGenerationCurrent;
        activeEntry.sha256 = m_cachedPdfLibrarySha256;
        activeEntry.displayName = m_cachedPdfLibraryDisplayName;
        activeEntry.publishedAt = m_cachedPdfLibraryPublishedAt;
        activeEntry.pdfStale = m_cachedPdfLibraryStale;
        activeEntry.latestBuildStatus = m_cachedPdfLibraryBuildStatus;
        activeEntry.latestBuildMessage = m_cachedPdfLibraryBuildMessage;
        activeEntry.lastCompletedBuildId = m_cachedPdfLibraryLastBuildId;
        activeEntry.localFreshnessAvailable = m_cachedPdfLibraryLocalFreshnessAvailable;
        activeEntry.localSourcesCurrent = m_cachedPdfLibraryLocalSourcesCurrent;
        activeEntry.localFreshnessMessage = m_cachedPdfLibraryLocalFreshnessMessage;
    }
    if (!m_pendingLibraryImportId.isEmpty()) {
        m_pdfLibraryAction->setIconText(i18n("Adding PDF…"));
        m_pdfLibraryAction->setIcon(QIcon::fromTheme(QStringLiteral("document-import")));
        m_pdfLibraryAction->setToolTip(i18n("Adding the PDF to the local Library and durable Sync queue"));
        m_pdfLibraryAction->setVisible(true);
        m_pdfLibraryAction->setEnabled(false);
        m_pdfLibraryAction->setData(QStringLiteral("adding-library-pdf"));
        updatePdfLibraryWidget(false);
        return;
    }
    if (!m_pendingLibraryRemovalId.isEmpty()) {
        m_pdfLibraryAction->setIconText(i18n("Removing PDF…"));
        m_pdfLibraryAction->setIcon(QIcon::fromTheme(QStringLiteral("edit-delete")));
        m_pdfLibraryAction->setToolTip(i18n("Removing the PDF locally and adding the change to the durable Sync queue"));
        m_pdfLibraryAction->setVisible(true);
        m_pdfLibraryAction->setEnabled(false);
        m_pdfLibraryAction->setData(QStringLiteral("removing-library-pdf"));
        updatePdfLibraryWidget(false);
        return;
    }
    if (!m_pendingSourceTarget.isEmpty()) {
        const QString phase = m_cachedFedoraRenderTarget == m_pendingSourceTarget ? m_cachedFedoraRenderStatus.value(QStringLiteral("status")).toString() : QStringLiteral("preparing");
        const QString reason = m_cachedFedoraRenderTarget == m_pendingSourceTarget ? m_cachedFedoraRenderStatus.value(QStringLiteral("reason")).toString().trimmed() : QString();
        QString label = i18n("Preparing source…");
        QString statusLabel = Thoughts::SyncStatusLabels::syncing();
        QString defaultReason = i18n("Compiling the source and recording exact build inputs without changing the active Library.");
        bool enabled = false;
        if (phase == QLatin1String("publishing")) {
            label = i18n("Publishing sources…");
            statusLabel = Thoughts::SyncStatusLabels::syncing();
            defaultReason = i18n("Publishing the source snapshot and exact build inputs to the server.");
        } else if (phase == QLatin1String("building")) {
            label = i18n("Sync in progress — click for output");
            statusLabel = Thoughts::SyncStatusLabels::syncing();
            defaultReason = i18n("The server is building the new source document. Click for live output.");
            enabled = true;
        } else if (phase == QLatin1String("activating")) {
            label = i18n("Activating Library…");
            statusLabel = Thoughts::SyncStatusLabels::syncing();
            defaultReason = i18n("Activating the complete authoritative Library generation from the server.");
        }
        m_pdfLibraryAction->setIconText(label);
        m_pdfLibraryAction->setIcon(QIcon::fromTheme(phase == QLatin1String("building") ? QStringLiteral("system-run") : QStringLiteral("document-send")));
        m_pdfLibraryAction->setToolTip(reason.isEmpty() ? defaultReason : reason);
        m_pdfLibraryAction->setVisible(true);
        m_pdfLibraryAction->setEnabled(enabled);
        m_pdfLibraryAction->setData(QStringLiteral("first-publication-%1").arg(phase));
        updatePdfLibraryWidget(false, statusLabel, reason.isEmpty() ? defaultReason : reason);
        return;
    }
    switch (state) {
    case PdfLibraryState::SourceLinked: {
        const QVariantMap activity = activeFedoraBuildActivity();
        const QVariantMap renderStatus = activeFedoraRenderStatus();
        QVariantMap pendingDocumentSync;
        QVariantMap manualSync;
        const int activeTab = m_tabWidget ? m_tabWidget->currentIndex() : -1;
        if (activeTab >= 0 && activeTab < m_tabs.size()) {
            QMetaObject::invokeMethod(m_tabs.at(activeTab).part, "documentSyncStatus", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, pendingDocumentSync), Q_ARG(QString, interactionTarget));
            QMetaObject::invokeMethod(m_tabs.at(activeTab).part, "fedoraManualSyncState", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, manualSync));
        }
        if (!manualSync.isEmpty()) {
            m_fedoraManualSyncState = manualSync.value(QStringLiteral("state")).toInt();
            m_fedoraManualSyncTarget = manualSync.value(QStringLiteral("target")).toString();
            m_fedoraManualSyncMessage = manualSync.value(QStringLiteral("message")).toString();
            m_fedoraManualSyncDetails = manualSync.value(QStringLiteral("details")).toString();
        }
        const QVariantMap remoteBuild = renderStatus.value(QStringLiteral("build")).toMap();
        const QString remoteBuildStatus = remoteBuild.value(QStringLiteral("status")).toString();
        const QString remoteBuildId = remoteBuild.value(QStringLiteral("id")).toString();
        if (!remoteBuildId.isEmpty() &&
            (remoteBuildStatus == QLatin1String("succeeded") || remoteBuildStatus == QLatin1String("attention") || remoteBuildStatus == QLatin1String("failed") || remoteBuildStatus == QLatin1String("cancelled"))) {
            m_cachedPdfLibraryLastBuildId = remoteBuildId;
        }

        Thoughts::DocumentSyncState syncState;
        syncState.buildTarget = !renderStatus.value(QStringLiteral("not_build_target")).toBool();
        syncState.libraryPdfStale = activeEntry.pdfStale;
        syncState.localFreshnessAvailable = activeEntry.localFreshnessAvailable;
        syncState.localSourcesCurrent = activeEntry.localSourcesCurrent;
        syncState.manualSyncState = (m_fedoraManualSyncTarget.isEmpty() || m_fedoraManualSyncTarget == interactionTarget) ? m_fedoraManualSyncState : 0;
        syncState.manualSyncMessage = m_fedoraManualSyncMessage;
        syncState.manualSyncDetails = m_fedoraManualSyncDetails;
        syncState.latestBuildMessage = activeEntry.latestBuildMessage;
        syncState.localFreshnessMessage = activeEntry.localFreshnessMessage;
        syncState.pendingDocumentSync = pendingDocumentSync;
        syncState.buildActivity = activity;
        syncState.renderStatus = renderStatus;
        const Thoughts::DocumentSyncPresentation presentation = Thoughts::documentSyncPresentation(syncState);
        m_pdfLibraryAction->setIconText(presentation.actionLabel);
        m_pdfLibraryAction->setIcon(QIcon::fromTheme(presentation.iconName));
        m_pdfLibraryAction->setToolTip(presentation.reason);
        m_pdfLibraryAction->setData(QStringLiteral("source-linked-%1").arg(presentation.actionId));
        m_pdfLibraryAction->setVisible(true);
        m_pdfLibraryAction->setEnabled(presentation.enabled);
        updatePdfLibraryWidget(true, presentation.status, presentation.reason, presentation.state);
        break;
    }
    case PdfLibraryState::SourceLinkedMissingLayout:
        m_pdfLibraryAction->setIconText(i18n("Re-render PDF"));
        m_pdfLibraryAction->setIcon(QIcon::fromTheme(QStringLiteral("view-refresh")));
        m_pdfLibraryAction->setToolTip(i18n("%1\nSource layout: missing. This PDF cannot be re-rendered until its source-layout record is restored.", publicationStatus(activeEntry)));
        m_pdfLibraryAction->setVisible(true);
        m_pdfLibraryAction->setEnabled(false);
        m_pdfLibraryAction->setData(QStringLiteral("source-linked-missing-layout"));
        updatePdfLibraryWidget(true, i18n("Error"), i18n("The source-layout record required to rebuild this PDF is missing."), Thoughts::DocumentSyncStatus::Error);
        break;
    case PdfLibraryState::UnlinkedLibrary:
        m_pdfLibraryAction->setIconText(i18n("Remove from Library…"));
        m_pdfLibraryAction->setIcon(QIcon::fromTheme(QStringLiteral("edit-delete")));
        m_pdfLibraryAction->setToolTip(i18n("%1\nSource layout: not linked. Remove this PDF from the shared computer and phone Library", publicationStatus(activeEntry)));
        m_pdfLibraryAction->setVisible(true);
        m_pdfLibraryAction->setEnabled(true);
        m_pdfLibraryAction->setData(QStringLiteral("unlinked-library"));
        updatePdfLibraryWidget(false);
        break;
    case PdfLibraryState::OutsideLibrary:
        // An earlier version of a Library PDF (from a card's History) is not added again.
        if (Thoughts::PdfVersions::isCachedVersion(activePdfUrl().toLocalFile())) {
            m_pdfLibraryAction->setIconText(i18n("Earlier Version"));
            m_pdfLibraryAction->setIcon(QIcon::fromTheme(QStringLiteral("document-open-recent")));
            m_pdfLibraryAction->setToolTip(i18n("An earlier version of a Library PDF, opened from a review card's History for reading only"));
            m_pdfLibraryAction->setVisible(true);
            m_pdfLibraryAction->setEnabled(false);
            m_pdfLibraryAction->setData(QStringLiteral("earlier-version"));
            updatePdfLibraryWidget(false);
            break;
        }
        m_pdfLibraryAction->setIconText(i18n("Add to Library…"));
        m_pdfLibraryAction->setIcon(QIcon::fromTheme(QStringLiteral("document-import")));
        m_pdfLibraryAction->setToolTip(i18n("Publish a source document or add the open external PDF to the shared computer and phone Library"));
        m_pdfLibraryAction->setVisible(true);
        m_pdfLibraryAction->setEnabled(true);
        m_pdfLibraryAction->setData(QStringLiteral("outside-library"));
        updatePdfLibraryWidget(false);
        break;
    case PdfLibraryState::NoDocument:
        m_pdfLibraryAction->setIconText(i18n("Add to Library…"));
        m_pdfLibraryAction->setIcon(QIcon::fromTheme(QStringLiteral("document-import")));
        m_pdfLibraryAction->setToolTip(i18n("Publish a source document or choose an external PDF for the shared computer and phone Library"));
        m_pdfLibraryAction->setVisible(true);
        m_pdfLibraryAction->setEnabled(true);
        m_pdfLibraryAction->setData(QStringLiteral("no-document"));
        updatePdfLibraryWidget(false);
        break;
    case PdfLibraryState::UnknownLibrary:
        m_pdfLibraryAction->setIconText(i18n("Library Status Unavailable"));
        m_pdfLibraryAction->setIcon(QIcon::fromTheme(QStringLiteral("dialog-warning")));
        m_pdfLibraryAction->setToolTip(i18n("The local Library index cannot safely identify this PDF."));
        m_pdfLibraryAction->setVisible(true);
        m_pdfLibraryAction->setEnabled(false);
        m_pdfLibraryAction->setData(QStringLiteral("unknown-library"));
        updatePdfLibraryWidget(false);
        break;
    }
}

void Shell::renderLinkedPdf()
{
    const int activeTab = m_tabWidget->currentIndex();
    const QUrl url = activePdfUrl();
    const bool currentSnapshot = m_pdfLibraryStateReady && m_pdfLibraryStateUrl == url;
    if (!currentSnapshot || m_cachedPdfLibraryState != PdfLibraryState::SourceLinked || m_cachedPdfLibraryRenderTarget.isEmpty()) {
        m_pdfLibraryStateReady = false;
        updatePdfLibraryAction();
        KMessageBox::error(this, i18n("The active PDF is not a source-linked canonical document."), i18n("Render Linked PDF"));
        return;
    }

    const QVariantMap activity = activeFedoraBuildActivity();
    if (activity.value(QStringLiteral("active")).toBool()) {
        if (activity.value(QStringLiteral("status")).toString() == QLatin1String("activating")) {
            return;
        }
        QString error;
        if (!m_buildTerminalLauncher ||
            !m_buildTerminalLauncher->open(activity.value(QStringLiteral("run_directory")).toString(), activity.value(QStringLiteral("watcher")).toString(), activity.value(QStringLiteral("server")).toString(), &error)) {
            KMessageBox::error(this, error.isEmpty() ? i18n("The server build output could not be opened.") : error, i18n("Build Output"));
        }
        return;
    }

    const QVariantMap renderStatus = activeFedoraRenderStatus();
    const QVariantMap activeBuild = renderStatus.value(QStringLiteral("build")).toMap();
    if (renderStatus.value(QStringLiteral("status")).toString() == QLatin1String("building") && !activeBuild.value(QStringLiteral("id")).toString().isEmpty()) {
        QString error;
        if (!m_buildTerminalLauncher ||
            !m_buildTerminalLauncher->openBuild(activeBuild.value(QStringLiteral("id")).toString(), QFileInfo(url.toLocalFile()).fileName(), Thoughts::Platform::nativeServerTool(QStringLiteral("thoughts-watch-build")), Thoughts::ServerAddress::configured(), &error)) {
            KMessageBox::error(this, error.isEmpty() ? i18n("The server build output could not be opened.") : error, i18n("Build Output"));
        }
        return;
    }

    const QString target = QFileInfo(url.toLocalFile()).fileName();
    const QString requestedAction = m_pdfLibraryAction ? m_pdfLibraryAction->data().toString() : QString();
    if (requestedAction == QLatin1String("source-linked-sync-now")) {
        triggerFedoraSyncWithInitiationControl(QStringLiteral("pdf_status_menu_sync"));
        return;
    }
    if (m_pendingRebuildAfterRefreshTarget == target) {
        return;
    }
    if (m_rebuildAuthorizedTarget == target || requestedAction == QLatin1String("source-linked-build-and-sync-now")) {
        m_pendingRebuildAfterRefreshTarget = target;
        m_pendingRebuildInitiationControl = QStringLiteral("pdf_rebuild_action");
        m_pendingRebuildSourcesCurrent = m_cachedPdfLibraryLocalFreshnessAvailable && m_cachedPdfLibraryLocalSourcesCurrent;
    }
    if (!QMetaObject::invokeMethod(m_tabs.at(activeTab).part, "requestFedoraRenderStatus", Qt::DirectConnection, Q_ARG(QString, target))) {
        m_pendingRebuildAfterRefreshTarget.clear();
        m_pendingRebuildInitiationControl.clear();
        m_pendingRebuildSourcesCurrent = false;
        KMessageBox::error(this, i18n("The server status service is unavailable for this document."), i18n("Sync PDF"));
        return;
    }
    updatePdfLibraryAction();
}

bool Shell::requestFedoraBuildForTarget(QObject *gateway, const QString &target, const QString &traceId)
{
    return gateway && !target.isEmpty() && QMetaObject::invokeMethod(gateway, "requestFedoraBuildForTarget", Qt::DirectConnection, Q_ARG(QString, target), Q_ARG(QString, traceId));
}

void Shell::openLastPdfBuild()
{
    const QUrl url = activePdfUrl();
    if (!url.isLocalFile() || m_cachedPdfLibraryLastBuildId.isEmpty()) {
        return;
    }
    QString error;
    if (!m_buildTerminalLauncher || !m_buildTerminalLauncher->openBuild(m_cachedPdfLibraryLastBuildId, QFileInfo(url.toLocalFile()).fileName(), Thoughts::Platform::nativeServerTool(QStringLiteral("thoughts-watch-build")), Thoughts::ServerAddress::configured(), &error)) {
        KMessageBox::error(this, error.isEmpty() ? i18n("The server build output could not be opened.") : error, i18n("Build Output"));
    }
}

void Shell::showSyncOperations()
{
    showSyncOperationLogs(QString());
}

void Shell::showSyncOperationLogs(const QString &requestedOperationId)
{
    if (m_syncOperationsDialog && !m_syncOperationsDialog->isVisible()) m_syncOperationsDialog = nullptr;
    if (m_syncOperationsDialog) {
        bool found = requestedOperationId.isEmpty();
        if (!requestedOperationId.isEmpty()) {
            if (auto *list = m_syncOperationsDialog->findChild<QListWidget *>(QStringLiteral("operationsList"))) {
                for (int row = 0; row < list->count(); ++row) {
                    if (list->item(row)->data(Qt::UserRole).toString() == requestedOperationId) {
                        list->setCurrentRow(row);
                        found = true;
                        break;
                    }
                }
            }
        }
        if (found) {
            m_syncOperationsDialog->show();
            m_syncOperationsDialog->raise();
            m_syncOperationsDialog->activateWindow();
            if (!requestedOperationId.isEmpty()) {
                if (auto *button = m_syncOperationsDialog->findChild<QPushButton *>(QStringLiteral("openOperationLog"))) button->click();
            }
            return;
        }
        auto *oldDialog = m_syncOperationsDialog.data();
        m_syncOperationsDialog = nullptr;
        oldDialog->close();
    }
    // The runs loaded so far, newest change first. The journal is read on its
    // own thread, one page at a time; a refresh asks for the newest runs and
    // the ones that changed, and only their rows are touched.
    struct LogsList {
        QList<QJsonObject> operations;
        // A row's text is kept until its run changes: formatting it costs more than moving it.
        QHash<QString, std::pair<QJsonObject, QString>> texts;
        QSet<QString> changed;
        // Where the next older page starts.
        qint64 oldestCreatedAtMs = 0;
        QString oldestId;
        bool hasOlder = false;
        bool loadingOlder = false;
        bool loaded = false;
    };
    auto state = std::make_shared<LogsList>();
    auto operations = std::shared_ptr<QList<QJsonObject>>(state, &state->operations);
    auto *dialog = new QDialog(this);
    m_syncOperationsDialog = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::NonModal);
    connect(dialog, &QDialog::finished, this, [this, dialog] {
        if (m_syncOperationLogDialog && m_syncOperationLogDialog->parentWidget() == dialog) m_syncOperationLogDialog->close();
        if (m_syncOperationsDialog == dialog) m_syncOperationsDialog = nullptr;
    });
    dialog->setObjectName(QStringLiteral("syncOperationsDialog"));
    dialog->setWindowTitle(i18n("Logs"));
    dialog->resize(620, 330);
    auto *layout = new QVBoxLayout(dialog);
    auto *list = new QListWidget(dialog);
    list->setObjectName(QStringLiteral("operationsList"));
    layout->addWidget(list, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    auto *olderButton = buttons->addButton(i18n("Show older"), QDialogButtonBox::ActionRole);
    olderButton->setObjectName(QStringLiteral("loadOlderOperations"));
    olderButton->setVisible(false);
    auto *resolveConflictButton = buttons->addButton(i18n("Resolve source conflict"), QDialogButtonBox::ActionRole);
    auto *retryButton = buttons->addButton(i18n("Retry"), QDialogButtonBox::ActionRole);
    auto *logsButton = buttons->addButton(i18n("Details"), QDialogButtonBox::ActionRole);
    logsButton->setObjectName(QStringLiteral("openOperationLog"));
    retryButton->setEnabled(false);
    resolveConflictButton->setVisible(false);
    logsButton->setEnabled(false);
    layout->addWidget(buttons);

    const auto rowText = [state](const QJsonObject &operation) {
        const QString id = operation.value(QStringLiteral("id")).toString();
        const auto cached = state->texts.constFind(id);
        if (cached != state->texts.cend() && cached->first == operation) {
            return cached->second;
        }
        const QString status = Thoughts::syncOperationOutcome(operation).title;
        const QString storedTarget = operation.value(QStringLiteral("target")).toString();
        const QString target = storedTarget.isEmpty() ? i18n("All documents") : storedTarget;
        const QString duration = Thoughts::syncOperationDurationLabel(operation);
        const QString initiation = syncInitiationControlLabel(operation.value(QStringLiteral("initiation_control")).toString());
        const QDateTime updated = QDateTime::fromMSecsSinceEpoch(operation.value(QStringLiteral("updated_at_ms")).toVariant().toLongLong()).toLocalTime();
        const QString summary = duration.isEmpty() ? QStringLiteral("%1 · %2").arg(status, target) : QStringLiteral("%1 · %2 · %3").arg(status, target, duration);
        const QString text = QStringLiteral("%1\n%2 · %3").arg(summary, initiation, QLocale().toString(updated, QLocale::ShortFormat));
        state->texts.insert(id, {operation, text});
        return text;
    };
    // Brings the list to the loaded runs without resetting it: rows that stay
    // keep their item, a changed run gets its new text, a new one is inserted.
    const auto populateList = [state, list, rowText](const QString &selectedId = {}) {
        const QSignalBlocker blocker(list);
        for (int row = list->count() - 1; row >= 0; --row) {
            if (list->item(row)->data(Qt::UserRole).toString().isEmpty()) {
                delete list->takeItem(row);
            }
        }
        for (int row = 0; row < state->operations.size(); ++row) {
            const QJsonObject &operation = state->operations.at(row);
            const QString id = operation.value(QStringLiteral("id")).toString();
            QListWidgetItem *item = row < list->count() ? list->item(row) : nullptr;
            if (!item || item->data(Qt::UserRole).toString() != id) {
                item = nullptr;
                for (int later = row + 1; later < list->count(); ++later) {
                    if (list->item(later)->data(Qt::UserRole).toString() == id) {
                        item = list->takeItem(later);
                        break;
                    }
                }
                if (!item) {
                    item = new QListWidgetItem;
                    item->setData(Qt::UserRole, id);
                }
                list->insertItem(row, item);
            }
            const QString text = rowText(operation);
            if (item->text() != text) {
                item->setText(text);
            }
        }
        while (list->count() > state->operations.size()) {
            delete list->takeItem(list->count() - 1);
        }
        if (state->operations.isEmpty()) {
            auto *item = new QListWidgetItem(state->loaded ? i18n("No recent operations") : i18n("Loading…"), list);
            item->setFlags(Qt::NoItemFlags);
            return;
        }
        int selectedRow = 0;
        for (int row = 0; !selectedId.isEmpty() && row < list->count(); ++row) {
            if (list->item(row)->data(Qt::UserRole).toString() == selectedId) {
                selectedRow = row;
                break;
            }
        }
        list->setCurrentRow(selectedRow);
    };
    // Adds or replaces runs; true when the list has to follow.
    const auto mergeOperations = [state](const QList<QJsonObject> &incoming) {
        bool changed = false;
        for (const QJsonObject &operation : incoming) {
            const QString id = operation.value(QStringLiteral("id")).toString();
            const auto found = std::find_if(state->operations.begin(), state->operations.end(), [&id](const QJsonObject &known) { return known.value(QStringLiteral("id")).toString() == id; });
            if (found == state->operations.end()) {
                state->operations.append(operation);
                changed = true;
            } else if (*found != operation) {
                *found = operation;
                changed = true;
            }
        }
        if (changed) {
            std::stable_sort(state->operations.begin(), state->operations.end(), [](const QJsonObject &left, const QJsonObject &right) {
                return left.value(QStringLiteral("updated_at_ms")).toVariant().toLongLong() > right.value(QStringLiteral("updated_at_ms")).toVariant().toLongLong();
            });
        }
        return changed;
    };
    populateList();

    const auto selectedOperation = [operations, list]() -> QJsonObject {
        const QListWidgetItem *item = list->currentItem();
        const QString id = item ? item->data(Qt::UserRole).toString() : QString();
        const auto found = std::find_if(operations->cbegin(), operations->cend(), [&id](const QJsonObject &operation) { return operation.value(QStringLiteral("id")).toString() == id; });
        return found == operations->cend() ? QJsonObject() : *found;
    };
    const auto updateSelection = [resolveConflictButton, retryButton, logsButton, selectedOperation]() {
        const QJsonObject operation = selectedOperation();
        if (operation.isEmpty()) {
            resolveConflictButton->setVisible(false);
            retryButton->setEnabled(false);
            logsButton->setEnabled(false);
            return;
        }
        resolveConflictButton->setVisible(syncOperationOffersSourceConflictResolution(operation));
        resolveConflictButton->setEnabled(syncOperationOffersSourceConflictResolution(operation));
        retryButton->setEnabled(Thoughts::syncOperationCanRetry(operation));
        logsButton->setEnabled(true);
    };
    connect(list, &QListWidget::currentItemChanged, dialog, [updateSelection](QListWidgetItem *, QListWidgetItem *) { updateSelection(); });
    connect(retryButton, &QPushButton::clicked, dialog, [this, dialog, selectedOperation] {
        const QString id = selectedOperation().value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) {
            dialog->accept();
            retrySyncOperation(id);
        }
    });
    connect(resolveConflictButton, &QPushButton::clicked, dialog, [this, dialog] {
        dialog->accept();
        beginSourceConflictResolution();
    });
    connect(logsButton, &QPushButton::clicked, dialog, [this, dialog, selectedOperation] {
        auto displayedOperation = std::make_shared<QJsonObject>(selectedOperation());
        const QString operationId = displayedOperation->value(QStringLiteral("id")).toString();
        if (operationId.isEmpty()) {
            return;
        }
        if (m_syncOperationLogDialog && !m_syncOperationLogDialog->isVisible()) m_syncOperationLogDialog = nullptr;
        if (m_syncOperationLogDialog) {
            if (m_syncOperationLogDialog->property("operationId").toString() == operationId) {
                m_syncOperationLogDialog->show();
                m_syncOperationLogDialog->raise();
                m_syncOperationLogDialog->activateWindow();
                return;
            }
            m_syncOperationLogDialog->close();
        }

        auto *logDialog = new QDialog(dialog);
        m_syncOperationLogDialog = logDialog;
        logDialog->setProperty("operationId", operationId);
        logDialog->setAttribute(Qt::WA_DeleteOnClose);
        logDialog->setWindowModality(Qt::NonModal);
        logDialog->setObjectName(QStringLiteral("syncOperationLogDialog"));
        const QString storedTarget = displayedOperation->value(QStringLiteral("target")).toString();
        const QString target = storedTarget.isEmpty() ? i18n("All documents") : storedTarget;
        logDialog->setWindowTitle(i18n("%1 · %2", Thoughts::syncOperationLifecycleTitle(*displayedOperation), target));
        logDialog->resize(760, 560);
        auto *logLayout = new QVBoxLayout(logDialog);
        auto *outcomeTitle = new QLabel(logDialog);
        outcomeTitle->setObjectName(QStringLiteral("operationOutcomeTitle"));
        outcomeTitle->setWordWrap(true);
        auto titleFont = outcomeTitle->font();
        titleFont.setBold(true);
        titleFont.setPointSize(titleFont.pointSize() + 2);
        outcomeTitle->setFont(titleFont);
        auto *outcomeMessage = new QLabel(logDialog);
        outcomeMessage->setObjectName(QStringLiteral("operationOutcomeMessage"));
        outcomeMessage->setTextFormat(Qt::PlainText);
        outcomeMessage->setTextInteractionFlags(Qt::TextSelectableByMouse);
        outcomeMessage->setWordWrap(true);
        outcomeMessage->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        auto *outcomeScroll = new QScrollArea(logDialog);
        outcomeScroll->setWidgetResizable(true);
        outcomeScroll->setFrameShape(QFrame::NoFrame);
        outcomeScroll->setWidget(outcomeMessage);
        auto *diagnosticsToggle = new QToolButton(logDialog);
        diagnosticsToggle->setObjectName(QStringLiteral("operationDiagnosticsToggle"));
        diagnosticsToggle->setText(i18n("Show diagnostics"));
        diagnosticsToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        diagnosticsToggle->setArrowType(Qt::RightArrow);
        diagnosticsToggle->setCheckable(true);
        auto *diagnostics = new QWidget(logDialog);
        diagnostics->setObjectName(QStringLiteral("operationDiagnostics"));
        auto *diagnosticsLayout = new QVBoxLayout(diagnostics);
        diagnosticsLayout->setContentsMargins(0, 0, 0, 0);
        diagnostics->hide();
        auto *technicalDetails = new QLabel(diagnostics);
        technicalDetails->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto *summary = new QLabel(logDialog);
        summary->setObjectName(QStringLiteral("operationLogSummary"));
        summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto *localChangesHeader = new QLabel(logDialog);
        localChangesHeader->setObjectName(QStringLiteral("operationLocalChangesHeader"));
        auto *localChanges = new QTreeWidget(logDialog);
        localChanges->setObjectName(QStringLiteral("operationLocalChanges"));
        localChanges->setColumnCount(3);
        localChanges->setHeaderLabels({i18n("Change"), i18n("File"), i18n("Size")});
        localChanges->setRootIsDecorated(false);
        localChanges->setUniformRowHeights(true);
        localChanges->setSelectionMode(QAbstractItemView::NoSelection);
        localChanges->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        localChanges->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        localChanges->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        auto *localChangesEmpty = new QLabel(i18n("No local source files changed."), logDialog);
        localChangesEmpty->setObjectName(QStringLiteral("operationLocalChangesEmpty"));
        auto *activityHeader = new QLabel(QStringLiteral("<b>%1</b>").arg(i18n("Sync activity").toHtmlEscaped()), logDialog);
        activityHeader->setObjectName(QStringLiteral("operationActivityHeader"));
        auto *logText = new QPlainTextEdit(logDialog);
        logText->setObjectName(QStringLiteral("operationLogText"));
        logText->setReadOnly(true);
        logLayout->addWidget(outcomeTitle);
        logLayout->addWidget(summary);
        logLayout->addWidget(outcomeScroll);
        auto *itemIssues = new QLabel(logDialog);
        itemIssues->setObjectName(QStringLiteral("syncItemIssues"));
        itemIssues->setTextFormat(Qt::RichText);
        itemIssues->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
        logLayout->addWidget(itemIssues);
        auto refreshIssues = [itemIssues] {
            auto *overview = commentsOverview();
            QStringList links;
            if (overview && overview->commentIssueCount() > 0)
                links << QStringLiteral("<a href='comments'>%1</a>").arg(i18np("1 comment has an issue", "%1 comments have issues", overview->commentIssueCount()).toHtmlEscaped());
            if (overview && overview->reviewCardIssueCount() > 0)
                links << QStringLiteral("<a href='cards'>%1</a>").arg(i18np("1 review card has an issue", "%1 review cards have issues", overview->reviewCardIssueCount()).toHtmlEscaped());
            if (const int mergeFiles = Thoughts::sourceFilesNeedingMerge(latestGlobalSyncOperationSummary()); mergeFiles > 0)
                links << QStringLiteral("<a href='source-merge'>%1</a>").arg(Thoughts::SyncStatusLabels::sourceFilesNeedMerge(mergeFiles).toHtmlEscaped());
            itemIssues->setText(links.join(QStringLiteral(" · ")));
            itemIssues->setVisible(!links.isEmpty());
        };
        refreshIssues();
        auto *issuesTimer = new QTimer(logDialog);
        connect(issuesTimer, &QTimer::timeout, logDialog, refreshIssues);
        issuesTimer->start(1000);
        connect(itemIssues, &QLabel::linkActivated, logDialog, [this, logDialog, dialog](const QString &kind) {
            if (kind == QLatin1String("source-merge")) {
                logDialog->accept();
                dialog->accept();
                showSourceFilesNeedingMerge();
                return;
            }
            auto *overview = commentsOverview();
            if (!overview) return;
            if (kind == QLatin1String("comments")) overview->showCommentIssues();
            else overview->showReviewCardsState(QStringLiteral("unavailable"));
            overview->show();
            overview->raise();
            overview->activateWindow();
            logDialog->accept();
            dialog->accept();
        });
        auto *diagnosticButtons = new QHBoxLayout;
        auto *diffButton = new QPushButton(i18n("Show diff"), logDialog);
        diffButton->setObjectName(QStringLiteral("operationShowDiff"));
        diffButton->setToolTip(i18n("Compare the last successful PDF source with the source used for this build."));
        diagnosticButtons->addWidget(diffButton);
        auto requestedDiffBuildId = std::make_shared<QString>();
        const auto failedBuilds = [displayedOperation] {
            QMap<QString, QString> failures;
            const auto add = [&failures](const QJsonObject &container) {
                for (const auto &value : container.value(QStringLiteral("failed_documents")).toArray()) {
                    const auto failure = value.toObject();
                    const QString target = failure.value(QStringLiteral("filename")).toString(failure.value(QStringLiteral("pdf_filename")).toString());
                    if (safeOperationPdfTarget(target)) failures.insert(target, failure.value(QStringLiteral("build_id")).toString());
                }
            };
            add(*displayedOperation);
            for (const auto &value : displayedOperation->value(QStringLiteral("events")).toArray()) {
                const auto metadata = value.toObject().value(QStringLiteral("metadata")).toObject();
                add(metadata);
                add(metadata.value(QStringLiteral("result")).toObject());
            }
            // Older journals recorded the failure without its originating build.
            bool needsLegacyStatus = false;
            for (auto it = failures.cbegin(); it != failures.cend(); ++it) needsLegacyStatus |= it.value().isEmpty();
            if (!needsLegacyStatus) return failures;
            const QDir stateRoot(QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server")));
            QFile snapshotFile(stateRoot.filePath(QStringLiteral("render-status.json")));
            const auto snapshot = snapshotFile.open(QIODevice::ReadOnly)
                ? QJsonDocument::fromJson(snapshotFile.readAll()).object() : QJsonObject();
            QJsonObject statuses;
            if (snapshot.value(QStringLiteral("schema_version")).toInt() == 1) {
                statuses = snapshot.value(QStringLiteral("render_statuses")).toObject();
            } else {
                QFile stateFile(stateRoot.filePath(QStringLiteral("repository.json")));
                if (stateFile.open(QIODevice::ReadOnly)) statuses = QJsonDocument::fromJson(stateFile.readAll()).object().value(QStringLiteral("render_statuses")).toObject();
            }
            for (auto it = failures.begin(); it != failures.end(); ++it) {
                if (it.value().isEmpty()) {
                    const auto status = statuses.value(it.key()).toObject();
                    it.value() = status.value(QStringLiteral("build_id")).toString(status.value(QStringLiteral("build")).toObject().value(QStringLiteral("id")).toString());
                }
            }
            return failures;
        };
        const auto chooseFailure = [logDialog, failedBuilds] {
            const auto targets = failedBuilds().keys();
            if (targets.size() == 1) return targets.first();
            if (targets.isEmpty()) return QString();
            return QInputDialog::getItem(logDialog, i18n("Choose document"), i18n("Document:"), targets, 0, false);
        };
        auto *retryBuildButton = new QPushButton(i18n("Retry build"), logDialog);
        diagnosticButtons->addWidget(retryBuildButton);
        connect(retryBuildButton, &QPushButton::clicked, logDialog, [this, logDialog, chooseFailure, operationId] {
            if (hasProcessManualSyncOperation() || !m_activeBuildOperationId.isEmpty()) return;
            const QString target = chooseFailure();
            if (target.isEmpty()) return;
            auto *gateway = fedoraGatewayPart();
            if (!gateway) return;
            m_activeBuildOperationId = beginSyncOperation(QStringLiteral("build"), target, QStringLiteral("waiting_source_publication"), QStringLiteral("logs_retry"), operationId);
            m_activeBuildOperationTarget = target;
            if (!requestFedoraBuildForTarget(gateway, target, m_activeBuildOperationId)) {
                updateSyncOperation(m_activeBuildOperationId, Thoughts::SyncOperationState::Failed, QStringLiteral("failed"), i18n("Build could not be started"));
                m_activeBuildOperationId.clear();
                m_activeBuildOperationTarget.clear();
            } else logDialog->accept();
        });
        connect(diffButton, &QPushButton::clicked, logDialog, [this, logDialog, displayedOperation, diffButton, failedBuilds, chooseFailure, requestedDiffBuildId] {
            KParts::ReadWritePart *part = fedoraGatewayPart();
            if (!part) {
                KMessageBox::error(logDialog, i18n("Open a Library document to retrieve this source comparison."));
                return;
            }
            connect(part, SIGNAL(fedoraBuildSourceDiffReady(QString,QJsonObject,QString)),
                    this, SLOT(showBuildSourceDiff(QString,QJsonObject,QString)), Qt::UniqueConnection);
            const auto failures = failedBuilds();
            const QString target = failures.isEmpty() ? QString() : chooseFailure();
            if (!failures.isEmpty() && target.isEmpty()) return;
            *requestedDiffBuildId = target.isEmpty() ? displayedOperation->value(QStringLiteral("build_id")).toString() : failures.value(target);
            if (requestedDiffBuildId->isEmpty()) return;
            diffButton->setEnabled(false);
            diffButton->setText(i18n("Loading diff…"));
            QMetaObject::invokeMethod(part, "requestFedoraBuildSourceDiff", Qt::QueuedConnection,
                                      Q_ARG(QString, *requestedDiffBuildId));
        });
        connect(this, &Shell::buildSourceDiffFinished, logDialog, [diffButton, requestedDiffBuildId](const QString &buildId) {
            if (buildId == *requestedDiffBuildId) {
                diffButton->setEnabled(true);
                diffButton->setText(i18n("Show diff"));
            }
        });
        diagnosticButtons->addWidget(diagnosticsToggle);
        auto *copyButton = new QPushButton(i18n("Copy diagnostics"), logDialog);
        copyButton->setObjectName(QStringLiteral("operationCopyDiagnostics"));
        diagnosticButtons->addWidget(copyButton);
        diagnosticButtons->addStretch();
        logLayout->addLayout(diagnosticButtons);
        logLayout->addWidget(diagnostics, 1);
        diagnosticsLayout->addWidget(technicalDetails);
        diagnosticsLayout->addWidget(localChangesHeader);
        diagnosticsLayout->addWidget(localChanges);
        diagnosticsLayout->addWidget(localChangesEmpty);
        diagnosticsLayout->addWidget(activityHeader);
        diagnosticsLayout->addWidget(logText, 1);
        auto *logButtons = new QDialogButtonBox(QDialogButtonBox::Close, logDialog);
        auto *fullBuildOutputButton = logButtons->addButton(i18n("Open full build output"), QDialogButtonBox::ActionRole);
        logLayout->addWidget(logButtons);

        const auto fitLogDialog = [logDialog, logLayout, outcomeMessage, outcomeScroll, diagnosticsToggle] {
            const int textWidth = qMax(1, logDialog->width() - logLayout->contentsMargins().left() - logLayout->contentsMargins().right());
            outcomeScroll->setFixedHeight(qBound(outcomeMessage->fontMetrics().height(), outcomeMessage->heightForWidth(textWidth), 160));
            logLayout->invalidate();
            logLayout->activate();
            if (!diagnosticsToggle->isChecked()) {
                logDialog->setFixedHeight(logLayout->totalSizeHint().height());
            }
        };
        connect(diagnosticsToggle, &QToolButton::toggled, logDialog, [diagnostics, diagnosticsToggle, logDialog, fitLogDialog](bool expanded) {
            logDialog->setMinimumHeight(0);
            logDialog->setMaximumHeight(QWIDGETSIZE_MAX);
            diagnostics->setVisible(expanded);
            diagnosticsToggle->setText(expanded ? i18n("Hide diagnostics") : i18n("Show diagnostics"));
            diagnosticsToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
            fitLogDialog();
            if (expanded) {
                logDialog->resize(logDialog->width(), qMax(560, logDialog->minimumSizeHint().height()));
            }
        });

        auto logContentDirty = std::make_shared<bool>(true);
        const auto refreshLog = [displayedOperation,
                                 logContentDirty,
                                 logDialog,
                                 summary,
                                 outcomeTitle,
                                 outcomeMessage,
                                 technicalDetails,
                                 localChangesHeader,
                                 localChanges,
                                 localChangesEmpty,
                                 logText,
                                 fullBuildOutputButton,
                                 diffButton,
                                 retryBuildButton,
                                 failedBuilds,
                                 fitLogDialog,
                                 operationId] {
            const QString storedTarget = displayedOperation->value(QStringLiteral("target")).toString();
            const QString target = storedTarget.isEmpty() ? i18n("All documents") : storedTarget;
            const QString traceId = displayedOperation->value(QStringLiteral("trace_id")).toString(operationId);
            const QString startKey = displayedOperation->contains(QStringLiteral("started_at_ms")) ? QStringLiteral("started_at_ms") : QStringLiteral("created_at_ms");
            const qint64 startedAtMs = displayedOperation->value(startKey).toVariant().toLongLong();
            const QDateTime startedAt = QDateTime::fromMSecsSinceEpoch(startedAtMs).toLocalTime();
            const QString elapsed = Thoughts::syncOperationElapsedLabel(*displayedOperation, QDateTime::currentMSecsSinceEpoch());
            const QStringList details {
                i18n("Started: %1", startedAtMs > 0 ? startedAt.toString(QStringLiteral("dd.MM.yyyy HH:mm:ss t")) : i18n("Not recorded")),
                i18n("Started from: %1", syncInitiationControlLabel(displayedOperation->value(QStringLiteral("initiation_control")).toString())),
                i18n("Elapsed: %1", elapsed.isEmpty() ? i18n("Not recorded") : elapsed),
                i18n("Uploaded: %1", Thoughts::syncOperationUploadedLabel(*displayedOperation)),
                i18n("Downloaded: %1", Thoughts::syncOperationDownloadedLabel(*displayedOperation)),
            };
            summary->setText(details.join(QLatin1Char('\n')));
            technicalDetails->setText(i18n("Trace ID: %1", traceId) + QLatin1Char('\n')
                + i18n("Server stream: %1", Thoughts::syncOperationServerStreamLabel(*displayedOperation)));
            // The elapsed timer changes only the summary. Reformat history and
            // rebuild widgets only when the selected operation actually changes.
            if (!*logContentDirty) {
                return;
            }
            *logContentDirty = false;
            const Thoughts::SyncOutcomePresentation outcome = Thoughts::syncOperationOutcome(*displayedOperation);
            logDialog->setWindowTitle(i18n("%1 · %2", outcome.title, target));
            outcomeTitle->setText(outcome.title);
            outcomeMessage->setText(outcome.message);
            const QList<Thoughts::SyncLocalChange> changes = Thoughts::syncOperationLocalChanges(*displayedOperation);
            localChangesHeader->setText(QStringLiteral("<b>%1</b>").arg(i18np("Local changes · %1 file", "Local changes · %1 files", changes.size()).toHtmlEscaped()));
            QStringList changeKeys;
            for (const Thoughts::SyncLocalChange &change : changes) {
                changeKeys.append(QStringLiteral("%1\n%2\n%3").arg(static_cast<int>(change.kind)).arg(change.path).arg(change.sizeBytes));
            }
            if (localChanges->property("thoughtsChangeKeys").toStringList() != changeKeys) {
                localChanges->setProperty("thoughtsChangeKeys", changeKeys);
                localChanges->clear();
                for (const Thoughts::SyncLocalChange &change : changes) {
                    auto *item = new QTreeWidgetItem(localChanges);
                    item->setText(0, Thoughts::syncLocalChangeKindLabel(change.kind));
                    item->setText(1, Thoughts::syncLocalChangeDisplayPath(changes, change));
                    item->setText(2, Thoughts::syncLocalChangeSizeLabel(change));
                    item->setToolTip(1, change.path);
                }
            }
            localChanges->setVisible(!changes.isEmpty());
            localChangesEmpty->setVisible(changes.isEmpty());
            if (!changes.isEmpty()) {
                const int rowHeight = qMax(20, localChanges->sizeHintForRow(0));
                const int visibleRows = qMin(5, changes.size());
                localChanges->setMaximumHeight(localChanges->header()->height() + visibleRows * rowHeight + localChanges->frameWidth() * 2 + 4);
            }
            const QString contents = Thoughts::syncOperationActivityLines(*displayedOperation).join(QLatin1Char('\n'));
            if (logText->toPlainText() != contents) {
                QScrollBar *scroll = logText->verticalScrollBar();
                const bool wasAtBottom = scroll->value() >= scroll->maximum() - 2;
                const int previousScroll = scroll->value();
                logText->setPlainText(contents);
                scroll->setValue(wasAtBottom ? scroll->maximum() : qMin(previousScroll, scroll->maximum()));
            }
            const auto failures = failedBuilds();
            bool hasFailureBuild = false;
            for (const auto &id : failures) hasFailureBuild |= !id.isEmpty();
            diffButton->setVisible(hasFailureBuild || !displayedOperation->value(QStringLiteral("build_id")).toString().isEmpty());
            retryBuildButton->setVisible(!failures.isEmpty());
            fullBuildOutputButton->setVisible(!displayedOperation->value(QStringLiteral("build_id")).toString().isEmpty() && !Thoughts::syncOperationBuildOutputTarget(*displayedOperation).isEmpty());
            fitLogDialog();
        };
        refreshLog();
        logText->setPlainText(i18n("Loading log…"));
        localChangesEmpty->hide();
        copyButton->setEnabled(false);

        connect(copyButton, &QPushButton::clicked, logDialog, [displayedOperation, summary, technicalDetails] {
            const Thoughts::SyncOutcomePresentation outcome = Thoughts::syncOperationOutcome(*displayedOperation);
            QApplication::clipboard()->setText(outcome.title + QStringLiteral("\n\n") + outcome.message + QStringLiteral("\n\n")
                + summary->text() + QLatin1Char('\n') + technicalDetails->text() + QStringLiteral("\n\n")
                + Thoughts::syncOperationReadableLog(*displayedOperation) + QStringLiteral("\n\n")
                + QString::fromUtf8(QJsonDocument(*displayedOperation).toJson(QJsonDocument::Indented)));
        });

        connect(fullBuildOutputButton, &QPushButton::clicked, logDialog, [this, logDialog, displayedOperation] {
            const QString buildId = displayedOperation->value(QStringLiteral("build_id")).toString();
            const QString target = Thoughts::syncOperationBuildOutputTarget(*displayedOperation);
            QString error;
            if (!buildId.isEmpty() && !target.isEmpty() && m_buildTerminalLauncher && m_buildTerminalLauncher->openBuild(buildId, target, Thoughts::Platform::nativeServerTool(QStringLiteral("thoughts-watch-build")), Thoughts::ServerAddress::configured(), &error)) {
                return;
            }
            KMessageBox::error(logDialog, error.isEmpty() ? i18n("The server build output could not be opened.") : error, i18n("Build Output"));
        });
        connect(logButtons, &QDialogButtonBox::rejected, logDialog, &QDialog::reject);

        auto *elapsedTimer = new QTimer(logDialog);
        elapsedTimer->setInterval(1000);
        connect(elapsedTimer, &QTimer::timeout, logDialog, refreshLog);
        elapsedTimer->start();

        auto *historyRefreshTimer = new QTimer(logDialog);
        historyRefreshTimer->setSingleShot(true);
        historyRefreshTimer->setInterval(100);
        // The run's history comes from the journal's thread. A refresh asks
        // only for the events after the last one read, however long the run.
        struct LoadedHistory {
            QJsonObject operation;
            QList<Thoughts::SyncJournal::Event> events;
            qint64 lastRow = 0;
            bool pending = false;
            bool again = false;
            bool asked = false;
        };
        auto history = std::make_shared<LoadedHistory>();
        auto requestHistory = std::make_shared<std::function<void()>>();
        const std::weak_ptr<std::function<void()>> requestHistoryAgain = requestHistory;
        *requestHistory = [history, requestHistoryAgain, logDialog, operationId, displayedOperation, logContentDirty, refreshLog, copyButton, logText] {
            if (history->pending) {
                history->again = true;
                return;
            }
            history->pending = true;
            // The window's first history normally arrives while it opens, as the
            // first page of the list does; a busy journal fills it in later.
            const int waitMs = std::exchange(history->asked, true) ? 0 : Thoughts::SyncJournal::LogsOpenWaitMs;
            syncJournal()->fetchHistory(logDialog, operationId, history->lastRow, [=](const Thoughts::SyncJournal::History &loaded) {
                history->pending = false;
                if (!loaded.found) {
                    logText->setPlainText(i18n("This log is no longer available."));
                    copyButton->setEnabled(false);
                } else if (!loaded.events.isEmpty() || loaded.operation != history->operation || !copyButton->isEnabled()) {
                    history->operation = loaded.operation;
                    for (const Thoughts::SyncJournal::Event &event : loaded.events) {
                        history->lastRow = qMax(history->lastRow, event.row);
                        history->events.append(event);
                    }
                    const QJsonObject refreshed = Thoughts::SyncJournal::operationFromHistory(loaded.operation, history->events);
                    if (refreshed != *displayedOperation || !copyButton->isEnabled()) {
                        *displayedOperation = refreshed;
                        *logContentDirty = true;
                        refreshLog();
                        copyButton->setEnabled(true);
                    }
                }
                if (std::exchange(history->again, false)) {
                    if (const auto again = requestHistoryAgain.lock()) {
                        (*again)();
                    }
                }
            }, waitMs);
        };
        connect(historyRefreshTimer, &QTimer::timeout, logDialog, [requestHistory] { (*requestHistory)(); });
        connect(this, &Shell::syncOperationJournalChanged, logDialog, [historyRefreshTimer, operationId](const QString &changedTraceId) {
            if (changedTraceId == operationId && !historyRefreshTimer->isActive()) {
                historyRefreshTimer->start(100);
            }
        });
        // Show the window before loading the selected history. The same timer
        // coalesces a trace replay into one refresh instead of one per event.
        historyRefreshTimer->start(0);
        QPointer<KParts::ReadWritePart> tracePart = fedoraGatewayPart();
        if (tracePart) {
            QMetaObject::invokeMethod(tracePart, "watchFedoraTrace", Qt::QueuedConnection, Q_ARG(QString, operationId), Q_ARG(qint64, displayedOperation->value(QStringLiteral("last_server_sequence")).toVariant().toLongLong()));
        }
        connect(logDialog, &QDialog::finished, this, [tracePart, operationId] {
            if (tracePart) QMetaObject::invokeMethod(tracePart, "stopWatchingFedoraTrace", Qt::QueuedConnection, Q_ARG(QString, operationId));
        });
        logDialog->show();
        logDialog->raise();
        logDialog->activateWindow();
    });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    auto *listRefreshTimer = new QTimer(dialog);
    listRefreshTimer->setSingleShot(true);
    listRefreshTimer->setInterval(100);
    const auto selectedId = [list] {
        const QListWidgetItem *selected = list->currentItem();
        return selected ? selected->data(Qt::UserRole).toString() : QString();
    };
    connect(listRefreshTimer, &QTimer::timeout, dialog, [state, dialog, mergeOperations, populateList, updateSelection, selectedId] {
        Thoughts::SyncJournal::PageRequest request;
        request.limit = SyncLogsRefreshRows;
        request.named = QStringList(state->changed.cbegin(), state->changed.cend());
        state->changed.clear();
        syncJournal()->fetchPage(dialog, request, [state, mergeOperations, populateList, updateSelection, selectedId](const Thoughts::SyncJournal::Page &page) {
            if (!state->loaded || !mergeOperations(page.operations + page.named)) {
                return;
            }
            populateList(selectedId());
            updateSelection();
        });
    });
    connect(this, &Shell::syncOperationJournalChanged, dialog, [listRefreshTimer, state](const QString &traceId) {
        state->changed.insert(traceId);
        if (!listRefreshTimer->isActive()) {
            listRefreshTimer->start();
        }
    });
    const auto loadOlder = [state, dialog, olderButton, mergeOperations, populateList, updateSelection, selectedId] {
        if (!state->hasOlder || state->loadingOlder) {
            return;
        }
        state->loadingOlder = true;
        Thoughts::SyncJournal::PageRequest request;
        request.beforeCreatedAtMs = state->oldestCreatedAtMs;
        request.beforeId = state->oldestId;
        syncJournal()->fetchPage(dialog, request, [state, olderButton, mergeOperations, populateList, updateSelection, selectedId](const Thoughts::SyncJournal::Page &page) {
            state->loadingOlder = false;
            state->hasOlder = page.hasOlder;
            if (!page.oldestId.isEmpty()) {
                state->oldestCreatedAtMs = page.oldestCreatedAtMs;
                state->oldestId = page.oldestId;
            }
            olderButton->setVisible(state->hasOlder);
            if (mergeOperations(page.operations)) {
                populateList(selectedId());
                updateSelection();
            }
        });
    };
    connect(olderButton, &QPushButton::clicked, dialog, loadOlder);
    // Reaching the end of the list loads the page before it.
    connect(list->verticalScrollBar(), &QScrollBar::valueChanged, dialog, [list, loadOlder](int value) {
        if (value > 0 && value >= list->verticalScrollBar()->maximum()) {
            loadOlder();
        }
    });
    updateSelection();
    // The first page normally arrives before the window shows; when the journal
    // is busy the window opens at once and fills in when the page is there.
    Thoughts::SyncJournal::PageRequest firstPage;
    if (!requestedOperationId.isEmpty()) {
        firstPage.named.append(requestedOperationId);
    }
    syncJournal()->fetchPage(dialog, firstPage, [state, olderButton, logsButton, mergeOperations, populateList, updateSelection, selectedOperation, requestedOperationId](const Thoughts::SyncJournal::Page &page) {
        state->loaded = true;
        state->hasOlder = page.hasOlder;
        state->oldestCreatedAtMs = page.oldestCreatedAtMs;
        state->oldestId = page.oldestId;
        olderButton->setVisible(state->hasOlder);
        mergeOperations(page.operations + page.named);
        populateList(requestedOperationId);
        updateSelection();
        if (!requestedOperationId.isEmpty() && selectedOperation().value(QStringLiteral("id")).toString() == requestedOperationId) {
            QTimer::singleShot(0, logsButton, &QPushButton::click);
        }
    }, Thoughts::SyncJournal::LogsOpenWaitMs);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void Shell::beginSourceConflictResolution()
{
    beginSourceConflictResolutionForPath(QString());
}

void Shell::showSourceFilesNeedingMerge()
{
    const QStringList paths = Thoughts::recordedSourceConflictPaths();
    if (paths.isEmpty()) {
        // Only the latest Sync reported the conflict; its file is not recorded.
        beginSourceConflictResolution();
        return;
    }
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("sourceMergeFilesDialog"));
    dialog->setWindowTitle(Thoughts::SyncStatusLabels::sourceFilesNeedMerge(paths.size()));
    auto *layout = new QVBoxLayout(dialog);
    auto *files = new QTreeWidget(dialog);
    files->setObjectName(QStringLiteral("sourceMergeFiles"));
    files->setHeaderLabels({i18n("Source file"), i18n("Why it needs a merge")});
    files->setRootIsDecorated(false);
    files->setUniformRowHeights(true);
    files->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    files->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    for (const QString &path : paths) {
        auto *item = new QTreeWidgetItem(files, {path, i18n("Changed on this computer and on the server since the last Sync, in overlapping places.")});
        item->setData(0, Qt::UserRole, path);
        item->setToolTip(1, i18n("The server kept both versions. Choose one or combine them, and Sync again."));
    }
    files->setCurrentItem(files->topLevelItem(0));
    layout->addWidget(files);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    auto *resolve = buttons->addButton(i18n("Resolve…"), QDialogButtonBox::AcceptRole);
    resolve->setObjectName(QStringLiteral("sourceMergeResolve"));
    resolve->setDefault(true);
    layout->addWidget(buttons);
    const auto resolveSelected = [this, dialog, files] {
        const QTreeWidgetItem *item = files->currentItem();
        if (!item) {
            return;
        }
        const QString path = item->data(0, Qt::UserRole).toString();
        dialog->accept();
        beginSourceConflictResolutionForPath(path);
    };
    connect(resolve, &QPushButton::clicked, dialog, resolveSelected);
    connect(files, &QTreeWidget::itemActivated, dialog, resolveSelected);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    dialog->resize(720, 320);
    dialog->open();
}

void Shell::beginSourceConflictResolutionForPath(const QString &path)
{
    KParts::ReadWritePart *part = fedoraGatewayPart();
    if (!part) {
        KMessageBox::error(this, i18n("Open a document from the Thoughts Library before resolving the source conflict."), i18n("Resolve Source Conflict"));
        return;
    }
    connectSourceConflictSignals(part);
    if (m_sourceConflictProgress) {
        m_sourceConflictProgress->close();
    }
    m_sourceConflictProgress = new QProgressDialog(i18n("Loading this computer's, the server's, and the common-base versions…"), QString(), 0, 0, this);
    m_sourceConflictProgress->setObjectName(QStringLiteral("sourceConflictProgress"));
    m_sourceConflictProgress->setWindowTitle(i18n("Resolve Source Conflict"));
    m_sourceConflictProgress->setCancelButton(nullptr);
    m_sourceConflictProgress->setWindowModality(modalityOver(this));
    m_sourceConflictProgress->setMinimumDuration(0);
    m_sourceConflictProgress->show();
    if (!QMetaObject::invokeMethod(part, "requestSourceConflictResolutionBundle", Qt::QueuedConnection, Q_ARG(QString, path))) {
        m_sourceConflictProgress->close();
        KMessageBox::error(this, i18n("This version of the document viewer cannot prepare source conflicts."), i18n("Resolve Source Conflict"));
    }
}

void Shell::connectSourceConflictSignals(KParts::ReadWritePart *part)
{
    connect(part,
            SIGNAL(sourceConflictResolutionBundleReady(QVariantMap,QString)),
            this,
            SLOT(sourceConflictResolutionBundleReady(QVariantMap,QString)),
            Qt::UniqueConnection); // clazy:exclude=old-style-connect
    connect(part,
            SIGNAL(sourceConflictResolutionStatusChanged(QString,QString,QString)),
            this,
            SLOT(sourceConflictResolutionStatusChanged(QString,QString,QString)),
            Qt::UniqueConnection); // clazy:exclude=old-style-connect
}

QString Shell::queueSourceConflictContents(const QVariantMap &bundle, const QByteArray &contents, QString *error)
{
    const QString validationError = sourceConflictContentsError(contents, bundle.value(QStringLiteral("path")).toString(), sourceConflictVersions(bundle));
    if (!validationError.isEmpty()) {
        *error = validationError;
        return {};
    }
    KParts::ReadWritePart *part = fedoraGatewayPart();
    if (!part) {
        *error = i18n("The Thoughts synchronization bridge is unavailable.");
        return {};
    }
    QString operationId;
    const bool invoked = QMetaObject::invokeMethod(part,
                                                    "queueSourceConflictResolution",
                                                    Qt::DirectConnection,
                                                    Q_RETURN_ARG(QString, operationId),
                                                    Q_ARG(QString, bundle.value(QStringLiteral("id")).toString()),
                                                    Q_ARG(qint64, bundle.value(QStringLiteral("expected_revision")).toLongLong()),
                                                    Q_ARG(QString, bundle.value(QStringLiteral("current_sha256")).toString()),
                                                    Q_ARG(QByteArray, contents));
    if (!invoked || operationId.isEmpty()) {
        *error = i18n("The resolution could not be saved to the durable synchronization queue.");
        return {};
    }
    return operationId;
}

bool Shell::queueSourceConflictResolution(const QVariantMap &bundle, const QByteArray &contents)
{
    QString error;
    if (queueSourceConflictContents(bundle, contents, &error).isEmpty()) {
        KMessageBox::error(this, error, i18n("Resolve Source Conflict"));
        return false;
    }
    KMessageBox::information(this,
                             i18n("The resolved file is safely queued. Thoughts will now submit it to the server and retry source publication."),
                             i18n("Resolution Saved"));
    triggerFedoraSync();
    return true;
}

QByteArray Shell::sourceConflictCommandContents(const QVariantMap &bundle, const QJsonObject &request, QString *error)
{
    const QString keep = request.value(QStringLiteral("keep")).toString();
    const QString contentFile = request.value(QStringLiteral("content_file")).toString();
    if (keep.isEmpty() == contentFile.isEmpty()) {
        *error = QStringLiteral("Give either keep (fedora or local) or content_file.");
        return {};
    }
    if (!contentFile.isEmpty()) {
        QFile file(contentFile);
        if (!QFileInfo(contentFile).isAbsolute() || !file.open(QIODevice::ReadOnly)) {
            *error = QStringLiteral("content_file must be an absolute path to a readable file.");
            return {};
        }
        return file.readAll();
    }
    if (keep != QLatin1String("fedora") && keep != QLatin1String("local")) {
        *error = QStringLiteral("keep must be fedora or local.");
        return {};
    }
    const QString key = keep == QLatin1String("fedora") ? QStringLiteral("fedora_content_base64") : QStringLiteral("mac_content_base64");
    return QByteArray::fromBase64(bundle.value(key).toString().toLatin1());
}

QJsonObject Shell::resolveSourceConflictCommand(const QJsonObject &request)
{
    const QString path = request.value(QStringLiteral("path")).toString();
    if (path.isEmpty()) {
        return Thoughts::syncCommandError(QStringLiteral("invalid_request"), QStringLiteral("path must name the conflicted source."));
    }
    KParts::ReadWritePart *part = fedoraGatewayPart();
    if (!part) {
        return Thoughts::syncCommandError(QStringLiteral("app_unavailable"), QStringLiteral("Open a document from the Thoughts Library before resolving the source conflict."));
    }
    if (m_sourceConflictCommandBundleHandler) {
        return Thoughts::syncCommandError(QStringLiteral("busy"), QStringLiteral("Another source conflict is being prepared."));
    }
    connectSourceConflictSignals(part);
    QEventLoop loop;
    QVariantMap bundle;
    QString error;
    bool ready = false;
    m_sourceConflictCommandBundleHandler = [&](const QVariantMap &result, const QString &resultError) {
        bundle = result;
        error = resultError;
        ready = true;
        loop.quit();
    };
    if (!QMetaObject::invokeMethod(part, "requestSourceConflictResolutionBundle", Qt::QueuedConnection, Q_ARG(QString, path))) {
        m_sourceConflictCommandBundleHandler = nullptr;
        return Thoughts::syncCommandError(QStringLiteral("app_unavailable"), QStringLiteral("This version of the document viewer cannot prepare source conflicts."));
    }
    // Shorter than the client's request timeout, so a slow download gets an answer.
    QTimer::singleShot(8000, &loop, &QEventLoop::quit);
    if (!ready) {
        loop.exec();
    }
    if (!ready) {
        // A late bundle must neither reach this finished call nor open the resolver.
        m_sourceConflictCommandBundleHandler = [](const QVariantMap &, const QString &) { };
        return Thoughts::syncCommandError(QStringLiteral("timeout"), QStringLiteral("The source conflict could not be downloaded in time."));
    }
    if (!error.isEmpty() || bundle.value(QStringLiteral("path")).toString() != path) {
        return Thoughts::syncCommandError(QStringLiteral("conflict_unavailable"), error.isEmpty() ? QStringLiteral("No unresolved source conflict is recorded for this path.") : error);
    }
    const QByteArray contents = sourceConflictCommandContents(bundle, request, &error);
    if (!error.isEmpty()) {
        return Thoughts::syncCommandError(QStringLiteral("invalid_request"), error);
    }
    const QString operationId = queueSourceConflictContents(bundle, contents, &error);
    if (operationId.isEmpty()) {
        return Thoughts::syncCommandError(QStringLiteral("invalid_request"), error);
    }
    triggerFedoraSync();
    return QJsonObject {{QStringLiteral("schema_version"), 1},
                        {QStringLiteral("accepted"), true},
                        {QStringLiteral("terminal"), true},
                        {QStringLiteral("state"), QStringLiteral("resolution_queued")},
                        {QStringLiteral("path"), path},
                        {QStringLiteral("conflict_id"), bundle.value(QStringLiteral("id")).toString()},
                        {QStringLiteral("operation_id"), operationId}};
}

void Shell::sourceConflictResolutionBundleReady(const QVariantMap &bundle, const QString &error)
{
    if (m_sourceConflictCommandBundleHandler) {
        std::exchange(m_sourceConflictCommandBundleHandler, nullptr)(bundle, error);
        return;
    }
    if (m_sourceConflictProgress) {
        m_sourceConflictProgress->close();
        m_sourceConflictProgress->deleteLater();
        m_sourceConflictProgress.clear();
    }
    if (!error.isEmpty() || bundle.isEmpty()) {
        KMessageBox::error(this, error.isEmpty() ? i18n("No unresolved source conflict is available.") : error, i18n("Resolve Source Conflict"));
        return;
    }

    const QByteArray baseContents = QByteArray::fromBase64(bundle.value(QStringLiteral("base_content_base64")).toString().toLatin1());
    const QByteArray fedoraContents = QByteArray::fromBase64(bundle.value(QStringLiteral("fedora_content_base64")).toString().toLatin1());
    const QByteArray macContents = QByteArray::fromBase64(bundle.value(QStringLiteral("mac_content_base64")).toString().toLatin1());
    const QByteArray mergedContents = QByteArray::fromBase64(bundle.value(QStringLiteral("merged_content_base64")).toString().toLatin1());
    if (baseContents.isNull() || fedoraContents.isNull() || macContents.isNull()) {
        KMessageBox::error(this, i18n("The downloaded source-conflict versions are incomplete."), i18n("Resolve Source Conflict"));
        return;
    }

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("sourceConflictResolver"));
    dialog.setWindowTitle(i18n("Resolve Source Conflict"));
    dialog.resize(620, 250);
    auto *layout = new QVBoxLayout(&dialog);
    auto *title = new QLabel(QStringLiteral("<b>%1</b>").arg(i18n("Choose the version the server should keep").toHtmlEscaped()), &dialog);
    layout->addWidget(title);
    const QString path = bundle.value(QStringLiteral("path")).toString();
    const int conflictCount = bundle.value(QStringLiteral("conflict_count")).toInt();
    auto *detail = new QLabel(i18n("%1 has %2 overlapping edits. Nothing will be overwritten without a guarded server check.", path, conflictCount),
                              &dialog);
    detail->setWordWrap(true);
    detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(detail);
    auto *times = new QLabel(i18n("This computer's edit: %1\nServer edit: %2",
                                  bundle.value(QStringLiteral("incoming_modified_at")).toString().isEmpty() ? i18n("time unavailable") : bundle.value(QStringLiteral("incoming_modified_at")).toString(),
                                  bundle.value(QStringLiteral("current_modified_at")).toString().isEmpty() ? i18n("time unavailable") : bundle.value(QStringLiteral("current_modified_at")).toString()),
                             &dialog);
    times->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(times);
    auto *explanation = new QLabel(i18n("Edit in Visual Studio Code opens one normal, fully editable file with THIS COMPUTER and SERVER conflict markers. Resolve them with VS Code’s accept actions or by hand and save: Thoughts submits the file as soon as it is complete."), &dialog);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    // A merge saved earlier can be submitted directly; this does not depend
    // on Visual Studio Code reporting that its tab was closed.
    const QString conflictId = bundle.value(QStringLiteral("id")).toString();
    const QFileInfo savedMergeInfo(sourceConflictResultPath(conflictId, path));
    QByteArray savedMerge;
    if (safeSourceConflictComponent(conflictId) && savedMergeInfo.isFile() && savedMergeInfo.size() <= 4 * 1024 * 1024) {
        QFile savedMergeFile(savedMergeInfo.absoluteFilePath());
        if (savedMergeFile.open(QIODevice::ReadOnly)) {
            savedMerge = savedMergeFile.readAll();
        }
    }
    const QString savedMergeError = savedMerge.isEmpty() ? QString() : sourceConflictContentsError(savedMerge, path, {fedoraContents, macContents});
    if (!savedMerge.isEmpty() && !savedMergeError.isEmpty()) {
        auto *savedMergeNote = new QLabel(i18n("Your saved merge is not ready to submit yet: %1 Edit it in Visual Studio Code to finish.", savedMergeError), &dialog);
        savedMergeNote->setObjectName(QStringLiteral("sourceConflictSavedMergeNote"));
        savedMergeNote->setWordWrap(true);
        layout->addWidget(savedMergeNote);
    }
    layout->addStretch();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    if (!savedMerge.isEmpty() && savedMergeError.isEmpty()) {
        auto *savedMergeButton = buttons->addButton(i18n("Submit saved merge (saved %1)", QLocale().toString(savedMergeInfo.lastModified(), QLocale::ShortFormat)),
                                                    QDialogButtonBox::AcceptRole);
        savedMergeButton->setObjectName(QStringLiteral("sourceConflictSubmitSavedMerge"));
        savedMergeButton->setDefault(true);
        connect(savedMergeButton, &QPushButton::clicked, &dialog, [this, &dialog, bundle, savedMerge] {
            const auto answer = QMessageBox::question(&dialog,
                                                      i18n("Use Saved Merge?"),
                                                      i18n("Submit the saved, marker-free merge to the server? The server will reject it safely if the file changed there since the conflict was prepared."),
                                                      QMessageBox::Yes | QMessageBox::Cancel,
                                                      QMessageBox::Yes);
            if (answer == QMessageBox::Yes && queueSourceConflictResolution(bundle, savedMerge)) {
                dialog.accept();
            }
        });
    }
    auto *vscodeButton = buttons->addButton(i18n("Edit conflict file in Visual Studio Code"), QDialogButtonBox::ActionRole);
    auto *keepMacButton = buttons->addButton(i18n("Keep this computer's version"), QDialogButtonBox::ActionRole);
    auto *keepFedoraButton = buttons->addButton(i18n("Keep the server's version"), QDialogButtonBox::ActionRole);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(keepMacButton, &QPushButton::clicked, &dialog, [this, &dialog, bundle, macContents] {
        if (queueSourceConflictResolution(bundle, macContents)) {
            dialog.accept();
        }
    });
    connect(keepFedoraButton, &QPushButton::clicked, &dialog, [this, &dialog, bundle, fedoraContents] {
        if (queueSourceConflictResolution(bundle, fedoraContents)) {
            dialog.accept();
        }
    });
    connect(vscodeButton, &QPushButton::clicked, &dialog, [this, &dialog, bundle, baseContents, fedoraContents, macContents, mergedContents] {
        const QString executable = visualStudioCodeExecutable();
        if (executable.isEmpty()) {
            KMessageBox::error(&dialog, i18n("Visual Studio Code could not be found."), i18n("Resolve Source Conflict"));
            return;
        }
        const QString conflictId = bundle.value(QStringLiteral("id")).toString();
        const QString root = sourceConflictMergeDirectory(conflictId);
        if (!QDir().mkpath(root)) {
            KMessageBox::error(&dialog, i18n("The protected merge workspace could not be created."), i18n("Resolve Source Conflict"));
            return;
        }
        const QString sourcePath = bundle.value(QStringLiteral("path")).toString();
        const QString suffix = sourceConflictSuffix(sourcePath);
        const QString basePath = QDir(root).filePath(QStringLiteral("Common base") + suffix);
        const QString macPath = QDir(root).filePath(QStringLiteral("This computer's version") + suffix);
        const QString fedoraPath = QDir(root).filePath(QStringLiteral("Server version") + suffix);
        const QString resultPath = sourceConflictResultPath(conflictId, sourcePath);
        const QByteArray initialResult = editableSourceConflictForTesting(mergedContents, macContents, fedoraContents);
        if (initialResult.isEmpty()) {
            KMessageBox::error(&dialog, i18n("The server’s merge result could not be converted into an editable conflict file."), i18n("Resolve Source Conflict"));
            return;
        }
        // The workspace belongs to this conflict. A merge saved in an earlier
        // attempt (for example before Thoughts was closed), finished or not,
        // is kept rather than replaced by a fresh conflict file; markers are
        // still checked when the editor closes.
        QFile previousResult(resultPath);
        const QByteArray previousContents = previousResult.open(QIODevice::ReadOnly) ? previousResult.readAll() : QByteArray();
        previousResult.close();
        const bool keepPreviousResult = !previousContents.isEmpty();
        if (!writeSourceConflictFile(basePath, baseContents) || !writeSourceConflictFile(macPath, macContents) || !writeSourceConflictFile(fedoraPath, fedoraContents)
            || (!keepPreviousResult && !writeSourceConflictFile(resultPath, initialResult))) {
            KMessageBox::error(&dialog, i18n("The protected merge files could not be written."), i18n("Resolve Source Conflict"));
            return;
        }
        // Saving a complete, marker-free merge is enough: it is submitted without returning to Thoughts.
        auto submitted = std::make_shared<bool>(false);
        auto rejected = std::make_shared<QByteArray>();
        auto *watcher = new QFileSystemWatcher({root, resultPath}, this);
        auto *settle = new QTimer(watcher);
        settle->setSingleShot(true);
        settle->setInterval(400);
        connect(watcher, &QFileSystemWatcher::fileChanged, settle, qOverload<>(&QTimer::start));
        connect(watcher, &QFileSystemWatcher::directoryChanged, settle, qOverload<>(&QTimer::start));
        connect(settle, &QTimer::timeout, this, [this, watcher, bundle, resultPath, sourcePath, submitted, rejected] {
            if (*submitted) {
                return;
            }
            // Editors that save by replacing the file drop it from the watch list.
            if (QFileInfo::exists(resultPath) && !watcher->files().contains(resultPath)) {
                watcher->addPath(resultPath);
            }
            QFile result(resultPath);
            if (!result.open(QIODevice::ReadOnly)) {
                return;
            }
            const QByteArray contents = result.read(4 * 1024 * 1024 + 1);
            if (contents.isEmpty() || contents == *rejected || sourceConflictHasMarkers(contents)) {
                return; // Still being resolved, or already reported.
            }
            const QString validationError = sourceConflictContentsError(contents, sourcePath, sourceConflictVersions(bundle));
            if (!validationError.isEmpty()) {
                *rejected = contents;
                KMessageBox::error(this, validationError + QStringLiteral("\n\n") + i18n("Fix the file in Visual Studio Code and save it again."), i18n("Resolve Source Conflict"));
                return;
            }
            *submitted = true;
            if (!queueSourceConflictResolution(bundle, contents)) {
                *submitted = false;
                *rejected = contents;
                return;
            }
            watcher->deleteLater();
        });
        auto *process = new QProcess(this);
        configureVisualStudioCode(process, executable, {QStringLiteral("--wait"), QStringLiteral("--goto"), QStringLiteral("%1:1:1").arg(resultPath)});
        connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError) {
            KMessageBox::error(this, i18n("Visual Studio Code could not be opened."), i18n("Resolve Source Conflict"));
            process->deleteLater();
        });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, process, bundle, resultPath, sourcePath, submitted, watcher = QPointer<QFileSystemWatcher>(watcher)](int exitCode, QProcess::ExitStatus status) {
            process->deleteLater();
            if (*submitted) {
                return; // Already submitted when it was saved.
            }
            if (watcher) {
                watcher->deleteLater();
            }
            if (status != QProcess::NormalExit || exitCode != 0) {
                KMessageBox::error(this, i18n("Visual Studio Code closed before the conflict file could be checked."), i18n("Resolve Source Conflict"));
                return;
            }
            QFile result(resultPath);
            if (!result.open(QIODevice::ReadOnly)) {
                KMessageBox::error(this, i18n("The resolved file could not be read."), i18n("Resolve Source Conflict"));
                return;
            }
            const QByteArray contents = result.read(4 * 1024 * 1024 + 1);
            const QString validationError = sourceConflictContentsError(contents, sourcePath, sourceConflictVersions(bundle));
            if (!validationError.isEmpty()) {
                KMessageBox::error(this, validationError + QStringLiteral("\n\n") + i18n("The merge workspace was preserved at %1.", resultPath), i18n("Resolve Source Conflict"));
                return;
            }
            QMessageBox confirmation(QMessageBox::Question,
                                     i18n("Use Resolved TeX?"),
                                     i18n("Submit the marker-free resolved file to the server? The server will reject it safely if the file changed there while the editor was open."),
                                     QMessageBox::NoButton,
                                     this);
            QPushButton *submit = confirmation.addButton(i18n("Use resolved file"), QMessageBox::AcceptRole);
            confirmation.addButton(QMessageBox::Cancel);
            confirmation.exec();
            if (confirmation.clickedButton() == submit) {
                queueSourceConflictResolution(bundle, contents);
            }
        });
        process->start();
        if (!process->waitForStarted(3000)) {
            return;
        }
        dialog.accept();
        KMessageBox::information(this,
                                 i18n("%1 is a normal editable file. Use VS Code’s Accept Current, Accept Incoming, or Accept Both actions—or edit it manually—then remove every conflict marker and save. Thoughts checks the file each time you save it and submits it to the server as soon as it is complete.", QFileInfo(resultPath).fileName()),
                                 i18n("Conflict File Opened"));
    });
    dialog.exec();
}

void Shell::sourceConflictResolutionStatusChanged(const QString &, const QString &status, const QString &message)
{
    if (status == QLatin1String("conflict")) {
        KMessageBox::error(this, message, i18n("Source Changed Again"));
    }
}

void Shell::retrySyncOperation(const QString &operationId)
{
    if (hasProcessManualSyncOperation() || !m_activeBuildOperationId.isEmpty()) {
        return;
    }
    const QJsonObject selected = readSyncOperationSummary(operationId);
    if (selected.isEmpty() || !Thoughts::syncOperationCanRetry(selected)) {
        return;
    }
    const int activeTab = m_tabWidget ? m_tabWidget->currentIndex() : -1;
    if (activeTab < 0 || activeTab >= m_tabs.size()) {
        return;
    }
    if (selected.value(QStringLiteral("kind")).toString() == QLatin1String("sync")) {
        const QString target = normalizedRetrySyncTarget(selected.value(QStringLiteral("target")).toString());
        const bool targeted = safeOperationPdfTarget(target);
        const LocalLibraryEntry targetEntry = targeted ? localLibraryEntry(ShellUtils::docsRoot(), target) : LocalLibraryEntry();
        const TargetSyncMode targetMode = targeted ? targetSyncMode(targetEntry.available, targetEntry.found, targetEntry.kind) : TargetSyncMode::SnapshotOnly;
        const QString retryOperationId = beginSyncOperation(QStringLiteral("sync"), target, QStringLiteral("queued"), QStringLiteral("logs_retry"), operationId);
        if (!setActiveManualSyncOperation(retryOperationId, QStringLiteral("logs_retry"))) {
            if (!retryOperationId.isEmpty()) {
                updateSyncOperation(retryOperationId, Thoughts::SyncOperationState::Failed, QStringLiteral("failed"), i18n("Another Thoughts sync is already active"));
            }
            return;
        }
        if ((!target.isEmpty() && !targeted) || targetMode == TargetSyncMode::Reconcile) {
            updateSyncOperation(
                m_activeManualSyncOperationId, Thoughts::SyncOperationState::Failed, QStringLiteral("failed"), i18n("The local Library cannot classify %1 yet. Run an all-documents sync to reconcile it with the server, then retry.", target));
            clearActiveManualSyncOperation();
            return;
        }
        const bool invoked = targeted ? dispatchTargetSync(m_tabs.at(activeTab).part, target, targetMode, m_activeManualSyncOperationId)
                                      : QMetaObject::invokeMethod(m_tabs.at(activeTab).part, "updateFromFedora", Qt::QueuedConnection, Q_ARG(QString, m_activeManualSyncOperationId));
        if (!invoked) {
            updateSyncOperation(m_activeManualSyncOperationId, Thoughts::SyncOperationState::Failed, QStringLiteral("failed"), i18n("Sync could not be started"));
            clearActiveManualSyncOperation();
        }
        return;
    }

    const QString target = selected.value(QStringLiteral("target")).toString();
    if (target.isEmpty() || !activePdfUrl().isLocalFile() || QFileInfo(activePdfUrl().toLocalFile()).fileName() != target || activePdfLibraryState() != PdfLibraryState::SourceLinked) {
        KMessageBox::information(this, i18n("Open %1, then retry this operation.", target), i18n("Retry Build"));
        return;
    }
    m_activeBuildOperationId = beginSyncOperation(QStringLiteral("build"), target, QStringLiteral("waiting_source_publication"), QStringLiteral("logs_retry"), operationId);
    m_activeBuildOperationTarget = target;
    m_rebuildAuthorizedTarget = target;
    m_pendingRebuildAfterRefreshTarget = target;
    if (!QMetaObject::invokeMethod(m_tabs.at(activeTab).part, "requestFedoraRenderStatus", Qt::DirectConnection, Q_ARG(QString, target))) {
        updateSyncOperation(m_activeBuildOperationId, Thoughts::SyncOperationState::Failed, QStringLiteral("failed"), i18n("Build could not be started"));
        m_activeBuildOperationId.clear();
        m_activeBuildOperationTarget.clear();
    }
}

void Shell::removePdfFromLibrary()
{
    const int activeTab = m_tabWidget ? m_tabWidget->currentIndex() : -1;
    if (activeTab < 0 || activeTab >= m_tabs.size()) {
        return;
    }
    const QUrl url = activePdfUrl();
    if (!m_pdfLibraryStateReady || m_pdfLibraryStateUrl != url || m_cachedPdfLibraryState != PdfLibraryState::UnlinkedLibrary) {
        m_pdfLibraryStateReady = false;
        updatePdfLibraryAction();
        return;
    }
    const QFileInfo pdf(url.toLocalFile());
    const QString expectedSha256 = m_cachedPdfLibrarySha256;
    if (expectedSha256.isEmpty()) {
        KMessageBox::error(this, i18n("The local Library index cannot safely identify this PDF."), i18n("Remove from Library"));
        return;
    }
    removePdfFromLibrary(pdf.absoluteFilePath(), pdf.fileName(), expectedSha256, m_cachedPdfLibraryDisplayName);
}

void Shell::removePdfFromLibrary(const QString &pdfPath, const QString &filename, const QString &expectedSha256, const QString &displayName)
{
    QWidget *dialogParent = s_commentsOverview && s_commentsOverview->isVisible() ? static_cast<QWidget *>(s_commentsOverview.data()) : this;
    const LocalLibraryEntry entry = localLibraryEntry(ShellUtils::docsRoot(), filename);
    const QString removalPath = QFileInfo(pdfPath).absoluteFilePath();
    const QString canonicalPath = QFileInfo(QDir(ShellUtils::docsRoot()).filePath(QStringLiteral("current_pdfs/%1").arg(filename))).absoluteFilePath();
    if (!entry.available || !entry.found || entry.kind != QLatin1String("unlinked") || entry.sha256 != expectedSha256 || removalPath != canonicalPath) {
        KMessageBox::error(dialogParent,
                           i18n("This document is no longer an unlinked Library PDF with the expected identity."),
                           i18n("Remove from Library"));
        return;
    }
    if (!m_pendingLibraryRemovalId.isEmpty()) {
        KMessageBox::information(dialogParent,
                                 i18n("Another Library removal is already pending."),
                                 i18n("Remove from Library"));
        return;
    }
    KParts::ReadWritePart *part = fedoraGatewayPart();
    if (!part) {
        KMessageBox::error(dialogParent,
                           i18n("Sync with the server is unavailable."),
                           i18n("Remove from Library"));
        return;
    }
    const QString documentName = displayName.trimmed().isEmpty() ? filename : displayName.trimmed();
    if (KMessageBox::questionTwoActions(
            dialogParent,
            i18n("Remove “%1” from the shared Library?\n\nThe document will disappear from this computer's Library immediately and the removal will be added to Local changes. The server and your phone remain unchanged until you click Sync. Its comments, replies, and review items will also be removed. Recovery copies and past review history will be retained.", documentName),
            i18n("Remove from Library"),
            KGuiItem(i18n("Remove from Library"), QStringLiteral("edit-delete")),
            KStandardGuiItem::cancel()) != KMessageBox::PrimaryAction) {
        return;
    }

    for (KMainWindow *window : KMainWindow::memberList()) {
        auto *shell = qobject_cast<Shell *>(window);
        if (!shell) {
            continue;
        }
        for (const TabState &tab : std::as_const(shell->m_tabs)) {
            if (tab.part->url().isLocalFile() && QFileInfo(tab.part->url().toLocalFile()).absoluteFilePath() == removalPath && !tab.part->queryClose()) {
                return;
            }
        }
    }

    m_pendingLibraryRemovalId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_pendingLibraryRemovalPath = removalPath;
    m_pendingLibraryRemovalSha256 = expectedSha256;
    const bool invoked = QMetaObject::invokeMethod(part, "removeLibraryPdf", Qt::DirectConnection, Q_ARG(QString, m_pendingLibraryRemovalId), Q_ARG(QString, filename), Q_ARG(QString, expectedSha256));
    if (!invoked) {
        m_pendingLibraryRemovalId.clear();
        m_pendingLibraryRemovalPath.clear();
        m_pendingLibraryRemovalSha256.clear();
        KMessageBox::error(dialogParent, i18n("Sync with the server is unavailable."), i18n("Remove from Library"));
    }
    updatePdfLibraryAction();
}

void Shell::handleLibraryRemovalFinished(const QString &requestId, const QString &filename, const QString &sha256, bool success, const QString &message)
{
    Q_UNUSED(filename)
    if (requestId.isEmpty() || requestId != m_pendingLibraryRemovalId) {
        return;
    }
    const QString expectedSha256 = m_pendingLibraryRemovalSha256;
    m_pendingLibraryRemovalId.clear();
    m_pendingLibraryRemovalPath.clear();
    m_pendingLibraryRemovalSha256.clear();
    m_pdfLibraryStateReady = false;
    if (!success || sha256 != expectedSha256) {
        updatePdfLibraryAction();
        KMessageBox::error(this, message.isEmpty() ? i18n("The local Library removal could not be queued.") : message, i18n("Remove from Library"));
        return;
    }

    for (KMainWindow *window : KMainWindow::memberList()) {
        if (auto *shell = qobject_cast<Shell *>(window)) {
            shell->m_pdfLibraryStateReady = false;
            shell->updatePdfLibraryAction();
        }
    }
    // The durable pending removal is the local Library tombstone.  Overview
    // applies it immediately while the immutable active generation remains
    // available for open readers and recovery.  Manual Sync publishes the
    // tombstone and atomically activates Fedora's replacement generation.
    updateCommentsOverviewDocumentSyncPresentations();
}

void Shell::showReviewOverview()
{
    if (!m_reviewOverview) {
        m_reviewOverview = new Thoughts::ReviewOverview(
            ShellUtils::docsRoot(),
            [this]() { return currentReviewDocument(); },
            [this](const Thoughts::ReviewCard &card, bool masked, QString *error) { return presentReviewCard(card, masked, error); },
            [this]() { clearReviewMasks(); },
            this,
            [this]() { return effectiveFedoraProjections(); },
            [this](const QString &stateFilter) {
                Thoughts::CommentsOverview *overview = commentsOverview();
                s_commentsOverviewShell = this;
                overview->showReviewCardsState(stateFilter);
                overview->setAttribute(Qt::WA_ShowWithoutActivating, false);
                overview->show();
                overview->raise();
                overview->activateWindow();
            },
            [this]() -> QWidget * {
                const int activeTab = m_tabWidget ? m_tabWidget->currentIndex() : -1;
                if (activeTab >= 0 && activeTab < m_tabs.size() && m_tabs.at(activeTab).part) {
                    return m_tabs.at(activeTab).part->widget();
                }
                return m_tabWidget;
            },
            m_fedoraManualSyncState == 1);
        // "Remove from this Card" on the card's agent conversations: queued like the Library's Remove.
        connect(m_reviewOverview, &Thoughts::ReviewOverview::agentConversationChangeRequested, this, [this](const QVariantMap &record) {
            bool queued = false;
            if (KParts::ReadWritePart *part = fedoraGatewayPart())
                QMetaObject::invokeMethod(part, "enqueueAgentConversation", Qt::DirectConnection, Q_RETURN_ARG(bool, queued), Q_ARG(QVariantMap, record));
            if (!queued) KMessageBox::error(m_reviewOverview, i18n("The conversation could not be removed: the local sync queue is not available."));
        });
        // The card's ‹ › arrows step through the answer areas shown in the active tab.
        const auto activeReviewPart = [this]() -> QObject * {
            const int activeTab = m_tabWidget ? m_tabWidget->currentIndex() : -1;
            return activeTab >= 0 && activeTab < m_tabs.size() ? m_tabs.at(activeTab).part : nullptr;
        };
        m_reviewOverview->setAnswerAreaNavigation(
            [activeReviewPart] {
                int count = 0;
                if (QObject *part = activeReviewPart()) QMetaObject::invokeMethod(part, "reviewMaskBlockCount", Qt::DirectConnection, Q_RETURN_ARG(int, count));
                return count;
            },
            [activeReviewPart](int index) {
                bool focused = false;
                if (QObject *part = activeReviewPart()) QMetaObject::invokeMethod(part, "focusReviewMaskBlock", Qt::DirectConnection, Q_RETURN_ARG(bool, focused), Q_ARG(int, index));
                return focused;
            });
        // The answer feedback as the reader finds it, for Copy for agent.
        m_reviewOverview->setAnswerFeedbackStates([activeReviewPart] {
            QVariantMap states;
            if (QObject *part = activeReviewPart()) QMetaObject::invokeMethod(part, "reviewFeedbackStates", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, states));
            return states;
        });
        // After a reload of its PDF, the card is shown again where the reader is, without raising anything.
        m_reviewOverview->setPresentationRestorer([this](const Thoughts::ReviewCard &card, QString *error) { return restoreReviewCard(card, error); });
        // A card's History is its document's, in the sidebar of the tab under review.
        connect(m_reviewOverview, &Thoughts::ReviewOverview::cardHistoryRequested, this, [this](const QString &cardId) { showCardHistory(cardId); });
        // The answer under review stays hidden there until it is revealed.
        connect(m_reviewOverview, &Thoughts::ReviewOverview::maskedHistoryCardChanged, this, [this] {
            for (const TabState &tab : std::as_const(m_tabs)) {
                if (!tab.part || !tab.part->widget()) continue;
                auto *panel = tab.part->widget()->findChild<Thoughts::ReviewFeedbackPanel *>(QStringLiteral("documentHistoryPanel"));
                if (panel && panel->isVisible()) updateDocumentHistory(tab.part, panel);
            }
        });
        connect(m_reviewOverview, &Thoughts::ReviewOverview::pdfVersionOpenRequested, this,
                [this](const QString &pdfPath, const QString &title, int page, const QVariantList &regions, bool current) {
                    QString error;
                    if (!openPdfVersion(pdfPath, title, page, regions, current, &error))
                        KMessageBox::error(this, error.isEmpty() ? i18n("The PDF could not be opened.") : error, i18n("Cannot Open Version"));
                });
        // The section note of a card follows the outline of its PDF, open in one of the tabs.
        m_reviewOverview->setContextOutline([this](const Thoughts::ReviewCard &card) {
            const QString pdfPath = QFileInfo(Thoughts::reviewCardPdfPath(ShellUtils::docsRoot(), card.record)).canonicalFilePath();
            if (pdfPath.isEmpty()) return QVariantList();
            const int activeTab = m_tabWidget ? m_tabWidget->currentIndex() : -1;
            QList<int> order;
            if (activeTab >= 0 && activeTab < m_tabs.size()) order.append(activeTab);
            for (int index = 0; index < m_tabs.size(); ++index) if (index != activeTab) order.append(index);
            for (int index : std::as_const(order)) {
                QObject *part = m_tabs.at(index).part;
                QString path;
                if (!part || !QMetaObject::invokeMethod(part, "localDocumentPath", Qt::DirectConnection, Q_RETURN_ARG(QString, path))) continue;
                if (QFileInfo(path).canonicalFilePath() != pdfPath) continue;
                QVariantList outline;
                QMetaObject::invokeMethod(part, "reviewDocumentOutline", Qt::DirectConnection, Q_RETURN_ARG(QVariantList, outline));
                return outline;
            }
            return QVariantList();
        });
        // Every window keeps its own review queue; a review recorded here must reach the others,
        // or they keep offering the card just reviewed.
        connect(m_reviewOverview, &Thoughts::ReviewOverview::reviewRecorded, this, [this] {
            for (KMainWindow *window : KMainWindow::memberList()) {
                auto *other = qobject_cast<Shell *>(window);
                if (other && other != this && other->m_reviewOverview) {
                    QTimer::singleShot(0, other->m_reviewOverview, &Thoughts::ReviewOverview::reloadFromProjections);
                }
            }
        });
        connect(m_reviewOverview, &Thoughts::ReviewOverview::reviewMaskInvertedChanged, this, [this](bool inverted) {
            for (const TabState &tab : std::as_const(m_tabs)) {
                QMetaObject::invokeMethod(tab.part, "setReviewMaskInverted", Qt::DirectConnection, Q_ARG(bool, inverted));
            }
        });
        connect(m_reviewOverview, &Thoughts::ReviewOverview::answerRevealed, this, [this] {
            for (const TabState &tab : std::as_const(m_tabs)) {
                QMetaObject::invokeMethod(tab.part, "revealReviewMask", Qt::DirectConnection);
            }
        });
        connect(m_reviewOverview, &Thoughts::ReviewOverview::reviewStarted, this, [this] {
            // Apply defaults once per session, after its PDF has opened.
            const int tab = m_tabWidget ? m_tabWidget->currentIndex() : -1;
            if (tab < 0 || tab >= m_tabs.size() || !m_tabs.at(tab).part) return;
            auto *actions = m_tabs.at(tab).part->actionCollection();
            for (const auto &name : {QStringLiteral("highlight_comments"), QStringLiteral("highlight_agent_edits")}) {
                if (QAction *action = actions->action(name)) action->setChecked(true);
            }
        });
        connect(m_reviewOverview, &Thoughts::ReviewOverview::cardInfoRequested, this, [this](const QString &cardId) {
            s_commentsOverviewShell = this;
            showReviewCardInLibrary(cardId);
        });
        connect(m_reviewOverview, &Thoughts::ReviewOverview::openSourceRequested, this, [this](const QString &sourcePath, int line) {
            QString error;
            if (!openSourceInVisualStudioCode(sourcePath, line, &error)) {
                KMessageBox::error(m_reviewOverview, error, i18n("Open Source in VS Code"));
            }
        });
    }
    m_reviewOverview->setSyncInProgress(m_fedoraManualSyncState == 1);
    m_reviewOverview->reload();
    m_reviewOverview->showLandingPage();
}

QVariantMap Shell::currentReviewDocument() const
{
    const int tab = m_tabWidget ? m_tabWidget->currentIndex() : -1;
    if (tab < 0 || tab >= m_tabs.size()) {
        return {{QStringLiteral("error"), i18n("Open a local documentation PDF first.")}};
    }
    QVariantMap result;
    const bool invoked = QMetaObject::invokeMethod(m_tabs.at(tab).part, "reviewDocumentPayload", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, result));
    if (!invoked) {
        result.insert(QStringLiteral("error"), i18n("This document view does not support review-card generation."));
    }
    return result;
}

// The sidebar's Open card: the card, its schedule and history in the Library's card browser.
void Shell::openSidebarReviewCard(const QString &cardId)
{
    showReviewCardInLibrary(cardId);
}

// The sidebar's Review now: the same single-card session as reviewing a card from the Library.
void Shell::reviewSidebarReviewCard(const QString &cardId)
{
    showReviewOverview();
    if (m_reviewOverview) m_reviewOverview->reviewCard(cardId);
}

void Shell::editSidebarReviewCard(const QString &cardId)
{
    QPointer<KParts::ReadWritePart> part = qobject_cast<KParts::ReadWritePart *>(sender());
    if (!part || !part->url().isLocalFile()) return;
    const QUrl url = part->url();
    Thoughts::ReviewStore store(ShellUtils::docsRoot(), [this] { return effectiveFedoraProjections(); });
    for (const auto &card : store.cards(nullptr, QFileInfo(url.toLocalFile()).completeBaseName())) {
        if (card.id() != cardId || card.schedule.deleted || card.pdfStem() != QFileInfo(url.toLocalFile()).completeBaseName()) continue;
        // On a document with editable source the edited areas move the card's source markers;
        // a card the server could not place is placed again on this PDF.
        if (!Thoughts::editReviewCardAreas(this, store, ShellUtils::docsRoot(), card, url.toLocalFile(),
                                           Thoughts::reviewCardNeedsPlacingAgain(card))) return;
        const auto projections = effectiveFedoraProjections();
        for (const auto &tab : std::as_const(m_tabs)) refreshSidebarReviewCards(tab.part, projections);
        if (part) QMetaObject::invokeMethod(part, "focusSidebarReviewCard", Qt::DirectConnection, Q_ARG(QString, cardId));
        if (s_commentsOverview) s_commentsOverview->reloadReviewCards();
        return;
    }
    KMessageBox::error(this, i18n("This review card is no longer available."), i18n("Cannot Edit Review Card"));
}

void Shell::mergeSidebarReviewCards(const QStringList &cardIds)
{
    mergeReviewCardsIn(qobject_cast<KParts::ReadWritePart *>(sender()), cardIds, false);
}

void Shell::mergeReviewCardsIn(KParts::ReadWritePart *readerPart, const QStringList &cardIds, bool afterSync)
{
    QPointer<KParts::ReadWritePart> part = readerPart;
    if (!part || !part->url().isLocalFile() || cardIds.size() < 2) return;
    // The server merges what it has: this Mac's unsent source goes first, by a Sync, and the merge
    // dialog opens when that Sync ends (a merge never runs during one: it would be read halfway).
    const auto mergeAfterSync = [this, part, &cardIds](bool start) {
        m_mergeAfterSync = PendingCardMerge{part, cardIds};
        QMetaObject::invokeMethod(part, "noticeMessage", Qt::DirectConnection,
                                  Q_ARG(QString, i18n("Sending your latest changes to the server first. The merge opens when Sync ends.")), Q_ARG(int, -1));
        if (start) triggerFedoraSyncWithInitiationControl(QStringLiteral("review_card_merge"));
        // A Sync that never starts (no server, say) still ends the wait: the merge says why.
        QTimer::singleShot(3 * 60 * 1000, this, [this, part] {
            if (!m_mergeAfterSync || m_mergeAfterSync->part != part || m_fedoraManualSyncState == 1) return;
            const PendingCardMerge pending = *m_mergeAfterSync;
            m_mergeAfterSync.reset();
            mergeReviewCardsIn(pending.part, pending.cardIds, true);
        });
    };
    if (m_fedoraManualSyncState == 1) {
        mergeAfterSync(false);
        return;
    }
    const QString stem = QFileInfo(part->url().toLocalFile()).completeBaseName();
    Thoughts::ReviewStore store(ShellUtils::docsRoot(), [this] { return effectiveFedoraProjections(); });
    QList<Thoughts::ReviewCard> cards;
    for (const auto &card : store.cards(nullptr, stem))
        if (cardIds.contains(card.id()) && !card.schedule.deleted) cards.append(card);
    if (cards.size() != cardIds.size()) {
        KMessageBox::error(this, i18n("A selected review card is no longer available."), i18n("Cannot Merge Review Cards"));
        return;
    }
    if (!afterSync) {
        bool syncFirst = false;
        if (!store.cardMergeProblem(cards, Thoughts::defaultReviewSourceBundleRoot(), nullptr, &syncFirst).isEmpty() && syncFirst) {
            mergeAfterSync(true);
            return;
        }
    }
    Thoughts::ReviewStore::CardMerge merged;
    if (!Thoughts::mergeReviewCards(this, store, cards, Thoughts::defaultReviewSourceBundleRoot(), &merged)) return;
    const auto projections = effectiveFedoraProjections();
    for (const auto &tab : std::as_const(m_tabs)) refreshSidebarReviewCards(tab.part, projections);
    if (part) QMetaObject::invokeMethod(part, "focusSidebarReviewCard", Qt::DirectConnection, Q_ARG(QString, merged.keptId));
    if (m_reviewOverview) m_reviewOverview->reload();
    if (s_commentsOverview) s_commentsOverview->reloadReviewCards();
}

void Shell::createHtmlReviewCard(const QString &pdfPath, const QVariantMap &fields)
{
    // A card from a selection in the HTML view: no PDF geometry; the server places its markers at
    // the next Sync (thoughts-html-view-SPEC.md, "New cards and comments from the HTML view").
    QVariantMap document{{QStringLiteral("pdf_path"), pdfPath},
                         {QStringLiteral("pdf_stem"), fields.value(QStringLiteral("pdf_stem"))},
                         {QStringLiteral("pdf_sha256"), fields.value(QStringLiteral("pdf_sha256"))},
                         {QStringLiteral("document_id"), fields.value(QStringLiteral("document_id"))}};
    const auto identity = Thoughts::DocumentIdentityStore(ShellUtils::docsRoot()).forFile(pdfPath, fields.value(QStringLiteral("pdf_sha256")).toString());
    if (!identity.value(QStringLiteral("document_id")).toString().isEmpty()) document.insert(QStringLiteral("document_id"), identity.value(QStringLiteral("document_id")).toString());
    if (document.value(QStringLiteral("document_id")).toString().isEmpty()) {
        KMessageBox::error(this, i18n("The selected text has no verified document identity."), i18n("Add Review Card"));
        return;
    }
    bool accepted = false;
    int firstDueDays = 2;
    QDateTime customFirstDueAt;
    const QString selected = fields.value(QStringLiteral("selected_text")).toString().simplified();
    const QString question = promptManualReviewQuestion(this, &accepted, &firstDueDays, &customFirstDueAt, {},
                                                        i18n("Answer: “%1”", selected.size() > 160 ? selected.left(157) + QStringLiteral("…") : selected));
    if (!accepted) return;
    QJsonObject html = QJsonObject::fromVariantMap(fields);
    for (const QString &key : {QStringLiteral("pdf_path"), QStringLiteral("pdf_stem"), QStringLiteral("pdf_sha256"), QStringLiteral("document_id"), QStringLiteral("selected_text")}) html.remove(key);
    Thoughts::ReviewStore store(ShellUtils::docsRoot(), [this]() { return effectiveFedoraProjections(); });
    Thoughts::ReviewCard card;
    QString error;
    // A selection in an HTML attachment page: anchored by its text and page state, never linked.
    const bool attachment = html.value(QStringLiteral("created_in")).toString() == QLatin1String("html_attachment");
    if (!(attachment ? store.createHtmlAttachmentCard(document, html, question, &card, &error, firstDueDays, customFirstDueAt)
                     : store.createHtmlCard(document, html, question, &card, &error, firstDueDays, customFirstDueAt))) {
        KMessageBox::error(this, error, i18n("Add Review Card"));
        return;
    }
    if (m_reviewOverview) m_reviewOverview->reload();
    if (s_commentsOverview) s_commentsOverview->reloadReviewCards();
    for (const auto &tab : std::as_const(m_tabs)) {
        if (tab.part && tab.part->url().isLocalFile() && QFileInfo(tab.part->url().toLocalFile()) == QFileInfo(pdfPath)) refreshSidebarReviewCards(tab.part, effectiveFedoraProjections());
    }
    if (attachment) {
        KMessageBox::information(this, i18n("Review card created. It is reviewed in this page, found by its text."), i18n("Review Card Added"));
        return;
    }
    KMessageBox::information(this, i18n("Review card created; placed in the PDF at the next Sync."), i18n("Review Card Added"));
}

void Shell::createManualReviewCard(const QString &pdfPath, int page, const QVariantList &rect, bool textSelection)
{
    createManualReviewCardAreas(pdfPath, {QVariantMap{{QStringLiteral("page"), page}, {QStringLiteral("rect"), rect}}}, textSelection);
}

void Shell::createManualReviewCardAreas(const QString &pdfPath, const QVariantList &requestedAreas, bool textSelection)
{
    if (m_manualReviewFingerprintRunning) {
        KMessageBox::information(this, i18n("Another review card is already being created."), i18n("Add Review Card"));
        return;
    }

    // Each area's page of the view, as a page of its document; every area must be on one document.
    QVariantMap document;
    QObject *const sourcePart = sender();
    QList<Thoughts::ReviewArea> areas;
    for (const QVariant &value : requestedAreas) {
        const QVariantMap requested = value.toMap();
        const QVariantList rect = requested.value(QStringLiteral("rect")).toList();
        int page = requested.value(QStringLiteral("page")).toInt();
        QVariantMap pageDocument;
        const bool invoked = sourcePart && QMetaObject::invokeMethod(sourcePart, "reviewDocumentPayloadForPage", Qt::DirectConnection,
            Q_RETURN_ARG(QVariantMap, pageDocument), Q_ARG(int, page));
        if (!invoked || !pageDocument.value(QStringLiteral("error")).toString().isEmpty()) {
            const QString error = pageDocument.value(QStringLiteral("error")).toString();
            KMessageBox::error(this, error.isEmpty() ? i18n("The PDF details could not be read.") : error, i18n("Add Review Card"));
            return;
        }
        if (document.isEmpty()) {
            document = pageDocument;
        } else if (pageDocument.value(QStringLiteral("pdf_path")) != document.value(QStringLiteral("pdf_path"))
                   || pageDocument.value(QStringLiteral("pdf_sha256")) != document.value(QStringLiteral("pdf_sha256"))) {
            KMessageBox::error(this, i18n("The selection runs over two documents. Select text within one document."), i18n("Add Review Card"));
            return;
        }
        page -= pageDocument.value(QStringLiteral("page_offset")).toInt();
        if (page < 1 || page > pageDocument.value(QStringLiteral("page_count")).toInt() || rect.size() != 4) {
            KMessageBox::error(this, i18n("The selected review-card area has no verified document identity."), i18n("Add Review Card"));
            return;
        }
        areas.append({page, QRectF(QPointF(rect[0].toDouble(), rect[1].toDouble()), QPointF(rect[2].toDouble(), rect[3].toDouble())).normalized(), textSelection});
    }
    const auto identity = document.isEmpty() ? QJsonObject()
        : Thoughts::DocumentIdentityStore(ShellUtils::docsRoot()).forFile(document.value(QStringLiteral("pdf_path")).toString(), document.value(QStringLiteral("pdf_sha256")).toString());
    if (identity.isEmpty() || areas.isEmpty()) {
        KMessageBox::error(this, i18n("The selected review-card area has no verified document identity."), i18n("Add Review Card"));
        return;
    }
    document.insert(QStringLiteral("document_id"), identity.value(QStringLiteral("document_id")).toString());
    Q_UNUSED(pdfPath)

    bool accepted = false;
    bool moreAreas = false;
    int firstDueDays = 2;
    QDateTime customFirstDueAt;
    const QString question = promptManualReviewQuestion(this, &accepted, &firstDueDays, &customFirstDueAt, {}, {}, &moreAreas);
    if (!accepted) {
        return;
    }

    const QString docsRoot = ShellUtils::docsRoot();
    const QString bundleRoot = document.value(QStringLiteral("source_bundle_root"), Thoughts::defaultReviewSourceBundleRoot()).toString();
    const QString path = document.value(QStringLiteral("pdf_path")).toString();
    const QString hash = document.value(QStringLiteral("pdf_sha256")).toString();
    const Thoughts::ReviewArea firstArea = areas.constFirst();
    // Saves the card with its linked areas, or (by hand) with the first area and a failed link.
    const auto finish = [this, document, path, hash, firstArea, firstDueDays, customFirstDueAt](const QString &cardQuestion, const Thoughts::ReviewAnchorPlan &plan,
                                                                                                const QString &cardId, QString visual) {
        const QJsonObject first = plan.regions.isEmpty() ? QJsonObject() : plan.regions.first().toObject();
        const int cardPage = first.isEmpty() ? firstArea.page : first.value(QStringLiteral("page")).toInt();
        const QVariantList cardRect = first.isEmpty()
            ? QVariantList{firstArea.rect.left(), firstArea.rect.top(), firstArea.rect.right(), firstArea.rect.bottom()}
            : first.value(QStringLiteral("rect")).toArray().toVariantList();
        if (!plan.pageVisualSha256.isEmpty()) visual = plan.pageVisualSha256;
        if (visual.isEmpty()) {
            const auto fingerprint = Thoughts::fingerprintReviewPage(path, cardPage, hash);
            if (fingerprint.contains(QStringLiteral("error"))) { KMessageBox::error(this, fingerprint.value(QStringLiteral("error")).toString(), i18n("Add Review Card")); return; }
            visual = fingerprint.value(QStringLiteral("page_visual_sha256")).toString();
        }
        Thoughts::ReviewStore store(ShellUtils::docsRoot(), [this]() { return effectiveFedoraProjections(); });
        Thoughts::ReviewCard card; QString error;
        if (!store.createManualCard(document, cardPage, cardRect, cardQuestion, visual, &card, &error, firstDueDays, customFirstDueAt, &plan, cardId)) {
            KMessageBox::error(this, error, i18n("Add Review Card")); return;
        }
        if (m_reviewOverview) m_reviewOverview->reload();
        if (s_commentsOverview) s_commentsOverview->reloadReviewCards();
        QString message = firstDueDays == -1 ? i18n("The review card was added and is due %1.", QLocale().toString(customFirstDueAt.toLocalTime(), QLocale::ShortFormat))
            : firstDueDays == 0 ? i18n("The review card was added and is due now.")
            : i18n("The review card was added and is due in %1 days.", firstDueDays);
        if (card.linkingState() == QLatin1String("linking_failed"))
            message += QStringLiteral("\n\n") + i18n("Linking failed: %1", card.linkingReason())
                + QStringLiteral("\n\n") + i18n("Open this card in the card browser and choose Edit placement to place its markers by hand.");
        else if (card.linkingState() == QLatin1String("pdf_without_source"))
            message += QStringLiteral("\n\n") + i18n("PDF without source location");
        else if (card.linkingState() == QLatin1String("pending_server_link"))
            message += QStringLiteral("\n\n") + i18n("Link pending: the server links the card to its source at the next Sync. It is not reviewed until then.");
        else if (plan.regions.size() > 1)
            message += QStringLiteral("\n\n") + i18np("Its answer area is linked to the source.", "Its %1 answer areas are linked to the source.", plan.regions.size());
        KMessageBox::information(this, message, i18n("Review Card Added"));
    };
    // The area editor: the card is saved only once every area is linked (or placed by hand).
    const auto chooseAreas = [this, docsRoot, bundleRoot, path, hash, areas, finish](const QString &cardQuestion, const QString &cardId) {
        Thoughts::NewReviewCardAreas chosen;
        if (!Thoughts::chooseNewReviewCardAreas(this, docsRoot, bundleRoot, path, hash, cardId, areas, cardQuestion, &chosen)) return;
        finish(chosen.question, chosen.plan, cardId, {});
    };
    if (moreAreas) {
        chooseAreas(question, QUuid::createUuid().toString(QUuid::WithoutBraces));
        return;
    }

    auto *progress = new QProgressDialog(i18n("Preparing the review card…"), i18n("Cancel"), 0, 0, this);
    progress->setWindowModality(modalityOver(this));
    progress->setAutoClose(false);
    m_manualReviewFingerprintRunning = true;
    struct PreparedCard {
        QVariantMap fingerprint;
        Thoughts::ReviewAnchorPlan plan;
        QString id;
        QString pdfIdentity;
    };
    auto *watcher = new QFutureWatcher<PreparedCard>(this);
    connect(progress, &QProgressDialog::canceled, watcher, [watcher] { watcher->setProperty("cancelled", true); });
    connect(watcher, &QFutureWatcher<PreparedCard>::finished, this, [this, watcher, progress, document, areas, question, finish, chooseAreas] {
        m_manualReviewFingerprintRunning = false;
        const auto result = watcher->result();
        const bool cancelled = watcher->property("cancelled").toBool();
        watcher->deleteLater(); progress->close(); progress->deleteLater();
        if (cancelled) return;
        if (result.fingerprint.contains(QStringLiteral("error"))) { KMessageBox::error(this, result.fingerprint.value(QStringLiteral("error")).toString(), i18n("Add Review Card")); return; }
        if (result.pdfIdentity.isEmpty() || result.pdfIdentity != Thoughts::artifactFileIdentity(document.value(QStringLiteral("pdf_path")).toString())) {
            KMessageBox::error(this, i18n("The PDF changed while preparing the card. Select the passage again."), i18n("Add Review Card"));
            return;
        }
        // A document with editable source gets no PDF-only card: an area that cannot be
        // linked is adjusted, or explicitly saved for placing its markers by hand (one area only:
        // markers placed by hand mark one passage).
        if (result.plan.invalidPdf && areas.size() > 1) {
            KMessageBox::error(this, result.plan.error, i18n("Add Review Card"));
            return;
        }
        if (!result.plan.invalidPdf && !result.plan.withoutSource && !result.plan.error.isEmpty()) {
            const int failed = areas.size() == 1 ? 0 : result.plan.failedArea;
            switch (Thoughts::askAboutUnlinkedArea(this, Thoughts::reviewAreaFailureText(areas, failed, result.plan.error), areas.size() == 1,
                                                   result.plan.sourceMapUnavailable)) {
            case Thoughts::ReviewAreaFailureChoice::Cancel:
                return;
            case Thoughts::ReviewAreaFailureChoice::LinkAtNextSync:
                finish(question, Thoughts::reviewPlanForServerLink(result.plan, areas, document.value(QStringLiteral("pdf_path")).toString(),
                                                                   document.value(QStringLiteral("pdf_sha256")).toString()),
                       result.id, result.fingerprint.value(QStringLiteral("page_visual_sha256")).toString());
                return;
            case Thoughts::ReviewAreaFailureChoice::AdjustAreas:
                chooseAreas(question, result.id);
                return;
            case Thoughts::ReviewAreaFailureChoice::PlaceByHand:
                break;
            }
        }
        finish(question, result.plan, result.id, result.fingerprint.value(QStringLiteral("page_visual_sha256")).toString());
    });
    watcher->setFuture(QtConcurrent::run([path, hash, areas, docsRoot, bundleRoot] {
        PreparedCard prepared;
        prepared.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        prepared.pdfIdentity = Thoughts::artifactFileIdentity(path);
        prepared.fingerprint = Thoughts::fingerprintReviewPage(path, areas.constFirst().page, hash);
        if (!prepared.fingerprint.contains(QStringLiteral("error")))
            prepared.plan = Thoughts::planReviewSourceAnchors(docsRoot, bundleRoot, path, hash, areas, prepared.id);
        return prepared;
    }));
    progress->show();
}

bool Shell::presentReviewCard(const Thoughts::ReviewCard &card, bool masked, QString *error)
{
    const bool cardBrowserWasVisible = !masked && s_commentsOverview && s_commentsOverview->isVisible();
    const QString pdfPath = Thoughts::reviewCardPdfPath(ShellUtils::docsRoot(), card.record);
    KParts::ReadWritePart *part = trackedCommentPart(pdfPath, true, error);
    if (!part) {
        return false;
    }
    if (!masked) {
        QMetaObject::invokeMethod(part, "clearReviewMask", Qt::DirectConnection);
        // A card made in an HTML attachment has no page: its page shows it.
        if (Thoughts::isHtmlAttachmentCard(card.record)) {
            bool shown = false;
            QMetaObject::invokeMethod(part, "showHtmlAttachmentCard", Qt::DirectConnection, Q_RETURN_ARG(bool, shown), Q_ARG(QJsonObject, card.record));
            showReviewCardPdfWindow(cardBrowserWasVisible);
            return true;
        }
        QMetaObject::invokeMethod(part, "goToPage", Qt::QueuedConnection, Q_ARG(uint, uint(card.page())));
        showReviewCardPdfWindow(cardBrowserWasVisible);
        return true;
    }
    if (!showReviewCardMask(part, card, true, error)) return false;
    showReviewCardPdfWindow(false);
    return true;
}

bool Shell::restoreReviewCard(const Thoughts::ReviewCard &card, QString *error)
{
    const QFileInfo pdf(Thoughts::reviewCardPdfPath(ShellUtils::docsRoot(), card.record));
    const int tab = pdf.isFile() ? findTabIndex(QUrl::fromLocalFile(pdf.absoluteFilePath())) : -1;
    if (tab < 0 || tab >= m_tabs.size() || !m_tabs.at(tab).part) {
        if (error) *error = i18n("The PDF of the review card is no longer open.");
        return false;
    }
    return showReviewCardMask(m_tabs.at(tab).part, card, false, error);
}

bool Shell::showReviewCardMask(KParts::ReadWritePart *part, const Thoughts::ReviewCard &card, bool navigate, QString *error)
{
    // The card's answer feedback, drawn once its answer is revealed, with the feedback of the other
    // cards of its document found in its answer (from cards that still exist).
    QMetaObject::invokeMethod(part, "setReviewFeedbackHomeCards", Qt::DirectConnection,
                              Q_ARG(QJsonObject, m_reviewOverview ? m_reviewOverview->feedbackHomeCards(card) : QJsonObject()));
    QMetaObject::invokeMethod(part, "setReviewFeedbackCard", Qt::DirectConnection, Q_ARG(QJsonObject, card.record), Q_ARG(QVariantList, card.regions()));
    if (m_reviewOverview) {
        connect(part, SIGNAL(reviewFeedbackChanged()), m_reviewOverview, SLOT(refreshAnswerFeedback()), Qt::UniqueConnection); // clazy:exclude=old-style-connect
        // A reload of the PDF (Sync rebuilt it) takes the mask away: the review presents its card again.
        connect(part, SIGNAL(reviewPresentationLost(QString)), m_reviewOverview, SLOT(restoreReviewPresentation(QString)), Qt::UniqueConnection); // clazy:exclude=old-style-connect
        // An HTML attachment card whose text left its page: the panel shows the stored answer.
        connect(part, SIGNAL(reviewAnswerNotFound(QString)), m_reviewOverview, SLOT(showAnswerNotFound(QString)), Qt::UniqueConnection); // clazy:exclude=old-style-connect
    }
    bool shown = false;
    const bool invoked = QMetaObject::invokeMethod(part, "showReviewMasks", Qt::DirectConnection, Q_RETURN_ARG(bool, shown), Q_ARG(QVariantList, card.regions()),
                                                   Q_ARG(bool, navigate));
    if (!invoked || !shown) {
        if (error) {
            *error = i18n("The answer mask could not be displayed.");
        }
        return false;
    }
    return true;
}

bool Shell::openPdfVersion(const QString &pdfPath, const QString &title, int page, const QVariantList &regions, bool current, QString *error)
{
    const bool cardBrowserWasVisible = s_commentsOverview && s_commentsOverview->isVisible();
    // Only the version cache: never a file of the Library opened under another title.
    if (!current && !Thoughts::PdfVersions::isCachedVersion(pdfPath)) {
        if (error) *error = i18n("This is not an earlier version of a Library PDF.");
        return false;
    }
    const bool alreadyOpen = findTabIndex(QUrl::fromLocalFile(QFileInfo(pdfPath).absoluteFilePath())) >= 0;
    KParts::ReadWritePart *part = trackedCommentPart(pdfPath, true, error);
    if (!part) return false;
    if (!current) {
        Thoughts::PdfVersions::setTitle(pdfPath, title);
        selectReaderTitle(part, {}, title);
        // An earlier version is not a file to come back to from Open Recent.
        if (!alreadyOpen && m_recent) m_recent->removeUrl(part->url());
    } else {
        QMetaObject::invokeMethod(part, "clearReviewMask", Qt::DirectConnection);
    }
    if (regions.isEmpty()) {
        if (page > 0) QMetaObject::invokeMethod(part, "goToPage", Qt::QueuedConnection, Q_ARG(uint, uint(page)));
    } else {
        QMetaObject::invokeMethod(part, "showPassageRegions", Qt::QueuedConnection, Q_ARG(QVariantList, regions));
    }
    showReviewCardPdfWindow(cardBrowserWasVisible);
    return true;
}

void Shell::showReviewCardPdfWindow(bool restoreCardBrowser)
{
    if (restoreCardBrowser && s_commentsOverview) {
        s_commentsOverview->show();
    }
    show();
    raise();
    activateWindow();
}

void Shell::clearReviewMasks()
{
    for (const TabState &tab : std::as_const(m_tabs)) {
        QMetaObject::invokeMethod(tab.part, "clearReviewMask", Qt::DirectConnection);
    }
}

KParts::ReadWritePart *Shell::fedoraGatewayPart() const
{
    const int activeTab = m_tabWidget ? m_tabWidget->currentIndex() : -1;
    if (activeTab >= 0 && activeTab < m_tabs.size()) {
        return m_tabs.at(activeTab).part;
    }
    return m_tabs.isEmpty() ? nullptr : m_tabs.constFirst().part;
}

QJsonObject Shell::effectiveFedoraProjections() const
{
    QJsonObject projections;
    KParts::ReadWritePart *const part = fedoraGatewayPart();
    if (part) {
        QMetaObject::invokeMethod(part, "effectiveFedoraProjections", Qt::DirectConnection, Q_RETURN_ARG(QJsonObject, projections));
    }
    return projections;
}

void Shell::refreshCommentsOverviewDocumentSyncStates()
{
    if (!s_commentsOverview) {
        return;
    }
    updateCommentsOverviewDocumentSyncPresentations();
}

void Shell::updateCommentsOverviewDocumentSyncPresentations()
{
    KParts::ReadWritePart *part = fedoraGatewayPart();
    QVariantMap manualSync;
    QVariantMap globalPendingChanges;
    if (part) {
        QMetaObject::invokeMethod(part, "fedoraManualSyncState", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, manualSync));
        QMetaObject::invokeMethod(part, "globalPendingChanges", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, globalPendingChanges));
    }
    if (!manualSync.isEmpty()) {
        m_fedoraManualSyncState = manualSync.value(QStringLiteral("state")).toInt();
        m_fedoraManualSyncTarget = manualSync.value(QStringLiteral("target")).toString();
        m_fedoraManualSyncMessage = manualSync.value(QStringLiteral("message")).toString();
        m_fedoraManualSyncDetails = manualSync.value(QStringLiteral("details")).toString();
    }
    if (globalPendingChanges.isEmpty()) {
        globalPendingChanges.insert(QStringLiteral("available"), false);
        globalPendingChanges.insert(QStringLiteral("total"), 0);
    }
    m_globalPendingChangesSnapshot = QJsonObject::fromVariantMap(globalPendingChanges);
    updatePdfSyncStatus();
    if (!s_commentsOverview) {
        return;
    }

    QJsonObject presentations;
    QList<Thoughts::DocumentSyncPresentation> aggregateDocuments;
    const QVariantList descriptors = s_commentsOverview->documentSyncDescriptors();
    for (const QVariant &descriptorValue : descriptors) {
        const QVariantMap descriptor = descriptorValue.toMap();
        const QString target = descriptor.value(QStringLiteral("filename")).toString().isEmpty() ? QFileInfo(descriptor.value(QStringLiteral("pdf_path")).toString()).fileName() : descriptor.value(QStringLiteral("filename")).toString();
        if (target.isEmpty()) {
            continue;
        }
        QVariantMap pendingDocumentSync;
        QVariantMap activity;
        QVariantMap renderStatus;
        if (part) {
            QMetaObject::invokeMethod(part, "documentSyncStatus", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, pendingDocumentSync), Q_ARG(QString, target));
            QMetaObject::invokeMethod(part, "fedoraBuildActivity", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, activity), Q_ARG(QString, target));
            QMetaObject::invokeMethod(part, "fedoraRenderStatus", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, renderStatus), Q_ARG(QString, target));
        }
        Thoughts::DocumentSyncState state;
        state.sourceLinked = descriptor.value(QStringLiteral("source_linked")).toBool();
        state.buildTarget = descriptor.value(QStringLiteral("build_target"), true).toBool()
            && !renderStatus.value(QStringLiteral("not_build_target")).toBool();
        state.libraryPdfStale = descriptor.value(QStringLiteral("library_pdf_stale")).toBool();
        state.deferLocalFreshness = state.sourceLinked;
        state.manualSyncState = (m_fedoraManualSyncTarget.isEmpty() || m_fedoraManualSyncTarget == target) ? m_fedoraManualSyncState : 0;
        state.manualSyncMessage = m_fedoraManualSyncMessage;
        state.manualSyncDetails = m_fedoraManualSyncDetails;
        state.latestBuildMessage = descriptor.value(QStringLiteral("latest_build_message")).toString();
        state.pendingDocumentSync = pendingDocumentSync;
        state.buildActivity = activity;
        state.renderStatus = renderStatus;
        const Thoughts::DocumentSyncPresentation presentation = Thoughts::documentSyncPresentation(state);
        presentations.insert(target, presentation.toJson());
        aggregateDocuments.append(presentation);
    }
    const Thoughts::AggregateSyncPresentation aggregate = Thoughts::aggregateSyncPresentation(aggregateDocuments);
    QJsonObject aggregateJson = aggregate.toJson();
    const bool syncBusy = m_fedoraManualSyncState == 1 || hasProcessManualSyncOperation() || !m_activeBuildOperationId.isEmpty();
    aggregateJson.insert(QStringLiteral("enabled"), aggregate.enabled && !syncBusy);
    s_commentsOverview->setSyncSnapshot(presentations,
                                        aggregateJson,
                                        m_globalPendingChangesSnapshot,
                                        lastSuccessfulGlobalSyncAt(),
                                        latestGlobalSyncOperationSummary());
}

void Shell::scheduleCommentsOverviewDocumentSyncPresentationUpdate()
{
    if (m_overviewPresentationRefreshScheduled) {
        return;
    }
    m_overviewPresentationRefreshScheduled = true;
    QTimer::singleShot(100, this, [this] {
        m_overviewPresentationRefreshScheduled = false;
        updateCommentsOverviewDocumentSyncPresentations();
    });
}

void Shell::startCommentsOverviewSync()
{
    triggerFedoraSyncWithInitiationControl(QStringLiteral("comments_overview_sync"));
}

Thoughts::CommentsOverview *Shell::commentsOverview()
{
    if (s_commentsOverview) {
        return s_commentsOverview;
    }

    const QString docsRoot = ShellUtils::docsRoot();
    auto *overview = new Thoughts::CommentsOverview(
        docsRoot,
        nullptr,
        [docsRoot](const Thoughts::ReviewCard &card, bool masked, QString *error) {
            const QString pdfPath = Thoughts::reviewCardPdfPath(docsRoot, card.record);
            if (Shell *shell = shellForCommentsOverview(pdfPath)) {
                return shell->presentReviewCard(card, masked, error);
            }
            if (error) {
                *error = i18n("No Thoughts PDF window is available.");
            }
            return false;
        },
        [](const QString &cardId) {
            if (Shell *shell = shellForCommentsOverview()) {
                shell->showReviewOverview();
                if (shell->m_reviewOverview) {
                    shell->m_reviewOverview->reviewCard(cardId);
                }
            }
        },
        [] {
            if (Shell *shell = shellForCommentsOverview()) {
                return shell->effectiveFedoraProjections();
            }
            return QJsonObject();
        });
    s_commentsOverview = overview;
    connect(overview, &Thoughts::CommentsOverview::noteAttachmentOpenRequested, overview, [overview](const QVariantMap &attachment) {
        if (Shell *shell = shellForCommentsOverview()) {
            if (KParts::ReadWritePart *part = shell->fedoraGatewayPart()) {
                QMetaObject::invokeMethod(part, "openNoteAttachment", Qt::DirectConnection, Q_ARG(QVariantMap, attachment));
                return;
            }
        }
        KMessageBox::error(overview, i18n("No Thoughts window is available to open this attachment."));
    });
    connect(overview, &Thoughts::CommentsOverview::syncRequested, overview, [] {
        if (Shell *shell = shellForCommentsOverview()) {
            shell->startCommentsOverviewSync();
            return;
        }
        if (s_commentsOverview) {
            s_commentsOverview->syncFinished(false, i18n("No Thoughts window is available for synchronization."));
        }
    });
    connect(overview, &Thoughts::CommentsOverview::reviewOverviewRequested, overview, [] {
        if (Shell *shell = shellForCommentsOverview()) {
            shell->showReviewOverview();
        }
    });
    connect(overview, &Thoughts::CommentsOverview::agentNotesRequested, overview, [] { Shell::showAgentNotes(); });
    {
        Thoughts::AgentNotes::Center *agentNotes = Thoughts::AgentNotes::Center::instance();
        connect(agentNotes, &Thoughts::AgentNotes::Center::unreadCountChanged, overview, &Thoughts::CommentsOverview::setAgentNotesUnread);
        overview->setAgentNotesUnread(agentNotes->hasListed() ? agentNotes->unreadCount() : 0);
    }
    connect(overview, &Thoughts::CommentsOverview::syncOperationsRequested, overview, [] {
        if (Shell *shell = shellForCommentsOverview()) {
            shell->showSyncOperations();
        }
    });
    connect(overview, &Thoughts::CommentsOverview::syncOperationLogsRequested, overview, [](const QString &operationId) {
        if (Shell *shell = shellForCommentsOverview()) {
            shell->showSyncOperationLogs(operationId);
        }
    });
    connect(overview, &Thoughts::CommentsOverview::sourceMergeRequested, overview, [] {
        if (Shell *shell = shellForCommentsOverview()) {
            shell->showSourceFilesNeedingMerge();
        }
    });
    connect(overview, &Thoughts::CommentsOverview::documentsChanged, overview, [] {
        if (Shell *shell = shellForCommentsOverview()) {
            shell->refreshCommentsOverviewDocumentSyncStates();
        }
    });
    connect(overview, &Thoughts::CommentsOverview::agentProposalsDecidedLocally, overview, [] { proposalDecided(); });
    connect(overview, &Thoughts::CommentsOverview::agentProposalsResolveRequested, overview, [overview](const QString &bundleId, const QString &action, const QStringList &proposalIds) {
        QString operationId;
        QString error;
        if (Shell *shell = shellForCommentsOverview()) {
            const int activeTab = shell->m_tabWidget ? shell->m_tabWidget->currentIndex() : -1;
            if (activeTab >= 0 && activeTab < shell->m_tabs.size()) {
                const bool invoked = QMetaObject::invokeMethod(
                    shell->m_tabs.at(activeTab).part, "resolveAgentProposals", Qt::DirectConnection, Q_RETURN_ARG(QString, operationId), Q_ARG(QString, bundleId), Q_ARG(QString, action), Q_ARG(QStringList, proposalIds));
                if (!invoked) {
                    error = i18n("The desktop proposal resolver is unavailable.");
                }
            }
        }
        if (operationId.isEmpty() && error.isEmpty()) {
            error = i18n("The proposal decision could not be written to the durable queue.");
        }
        overview->completeAgentProposalResolution(proposalIds, !operationId.isEmpty(), error);
    });
    if (Shell *shell = shellForCommentsOverview()) {
        const int activeTab = shell->m_tabWidget ? shell->m_tabWidget->currentIndex() : -1;
        if (activeTab >= 0 && activeTab < shell->m_tabs.size()) {
            QJsonObject projections;
            QVariantMap syncState;
            QVariantMap renderState;
            QMetaObject::invokeMethod(shell->m_tabs.at(activeTab).part, "effectiveFedoraProjections", Qt::DirectConnection, Q_RETURN_ARG(QJsonObject, projections));
            QMetaObject::invokeMethod(shell->m_tabs.at(activeTab).part, "fedoraManualSyncState", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, syncState));
            QMetaObject::invokeMethod(shell->m_tabs.at(activeTab).part, "fedoraRenderState", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, renderState));
            overview->setFedoraProjections(projections);
            overview->setPdfSyncState(QJsonObject::fromVariantMap(renderState));
            shell->updateCommentsOverviewManualSyncState(syncState.value(QStringLiteral("state")).toInt(), syncState.value(QStringLiteral("message")).toString(), syncState.value(QStringLiteral("details")).toString());
        }
    }
    connect(overview, &Thoughts::CommentsOverview::pdfVersionOpenRequested, overview,
            [overview](const QString &pdfPath, const QString &title, int page, const QVariantList &regions, bool current) {
                QString error;
                Shell *shell = shellForCommentsOverview(current ? pdfPath : QString());
                if (!shell) error = i18n("No Thoughts PDF window is available.");
                else if (shell->openPdfVersion(pdfPath, title, page, regions, current, &error)) return;
                KMessageBox::error(overview, error.isEmpty() ? i18n("The PDF could not be opened.") : error, i18n("Cannot Open Version"));
            });
    connect(overview, &Thoughts::CommentsOverview::openCodexRequested, overview, [overview](const QString &pdfPath, const QString &annotationId) { openCodexForComment(overview, pdfPath, annotationId); });
    connect(overview, &Thoughts::CommentsOverview::openSourceRequested, overview, [overview](const QString &sourcePath, int line) {
        QString error;
        if (!openSourceInVisualStudioCode(sourcePath, line, &error)) {
            KMessageBox::error(overview, error, i18n("Open Source in VS Code"));
        }
    });
    connect(overview, &Thoughts::CommentsOverview::sourceMonitoringInformationRequested, overview, [overview](const QString &pdfPath) {
        QVariantMap information {
            {QStringLiteral("schema_version"), 1},
            {QStringLiteral("target"), QFileInfo(pdfPath).fileName()},
            {QStringLiteral("status"), QStringLiteral("unavailable")},
            {QStringLiteral("error"), i18n("No Thoughts PDF window is available for local source monitoring.")},
        };
        if (Shell *shell = shellForCommentsOverview(pdfPath)) {
            if (KParts::ReadWritePart *part = shell->fedoraGatewayPart()) {
                QMetaObject::invokeMethod(part,
                                          "desktopSourceMonitoringInformation",
                                          Qt::DirectConnection,
                                          Q_RETURN_ARG(QVariantMap, information),
                                          Q_ARG(QString, QFileInfo(pdfPath).fileName()));
            }
        }
        overview->applySourceMonitoringInformation(pdfPath, information);
        if (information.value(QStringLiteral("status")).toString()
            == QLatin1String("reconciling")) {
            QTimer::singleShot(750, overview, [overview, pdfPath] {
                if (overview->sourceMonitoringRefreshNeeded(pdfPath)) {
                    Q_EMIT overview->sourceMonitoringInformationRequested(pdfPath);
                }
            });
        }
    });
    connect(overview, &Thoughts::CommentsOverview::sourceMonitoringBatchRequested, overview, [overview](quint64 requestId, const QStringList &targets) {
        if (Shell *shell = shellForCommentsOverview()) {
            if (KParts::ReadWritePart *part = shell->fedoraGatewayPart()) {
                if (QMetaObject::invokeMethod(part, "requestDesktopSourceMonitoringInformation", Qt::DirectConnection,
                                              Q_ARG(QStringList, targets), Q_ARG(QObject *, overview), Q_ARG(quint64, requestId))) { return; }
            }
        }
        overview->applySourceMonitoringBatch(requestId, {});
    });
    connect(overview, &Thoughts::CommentsOverview::documentActivated, overview, [](const QString &pdfPath) {
        if (Shell *shell = shellForCommentsOverview(pdfPath)) {
            shell->openTrackedDocument(pdfPath);
        }
    });
    connect(overview, &Thoughts::CommentsOverview::documentPageActivated, overview, [](const QString &pdfPath, int page, const QString &documentId, const QString &displayTitle, const QString &projectId) {
        if (Shell *shell = shellForCommentsOverview(pdfPath)) {
            QString error;
            KParts::ReadWritePart *part = shell->trackedCommentPart(pdfPath, true, &error);
            if (!part) {
                KMessageBox::error(shell, error);
                return;
            }
            shell->selectReaderTitle(part, documentId, displayTitle, projectId);
            if (page > 0) {
                QMetaObject::invokeMethod(part, "goToPage", Qt::QueuedConnection, Q_ARG(uint, uint(page)));
            }
            shell->show();
            shell->raise();
            shell->activateWindow();
        }
    });
    connect(overview, &Thoughts::CommentsOverview::librarySearchPageActivated, overview, [](const QString &pdfPath, int page, const QString &query) {
        if (Shell *shell = shellForCommentsOverview(pdfPath)) {
            QString error;
            KParts::ReadWritePart *part = shell->trackedCommentPart(pdfPath, true, &error);
            if (!part) {
                KMessageBox::error(shell, error);
                return;
            }
            shell->selectReaderTitle(part, QString(), QString(), QString());
            if (page > 0) {
                QMetaObject::invokeMethod(part, "goToPage", Qt::QueuedConnection, Q_ARG(uint, uint(page)));
            }
            QMetaObject::invokeMethod(part, "showLibrarySearch", Qt::QueuedConnection, Q_ARG(QString, query), Q_ARG(int, page));
            shell->show();
            shell->raise();
            shell->activateWindow();
        }
    });
    connect(overview, &Thoughts::CommentsOverview::commentActivated, overview, [](const QString &pdfPath, const QString &annotationId, int page) {
        if (Shell *shell = shellForCommentsOverview(pdfPath)) {
            shell->openTrackedComment(pdfPath, annotationId, page);
        }
    });
    connect(overview, &Thoughts::CommentsOverview::documentFeedbackActivated, overview, [](const QString &pdfPath, const QString &feedbackId) {
        if (Shell *shell = shellForCommentsOverview(pdfPath)) shell->openTrackedFeedback(pdfPath, feedbackId);
    });
    connect(overview, &Thoughts::CommentsOverview::documentFeedbackWriteRequested, overview, [](const QVariantMap &record) {
        bool queued = false;
        if (Shell *shell = shellForCommentsOverview()) {
            if (KParts::ReadWritePart *part = shell->fedoraGatewayPart()) {
                QMetaObject::invokeMethod(part, "enqueueDocumentFeedbackRecord", Qt::DirectConnection, Q_RETURN_ARG(bool, queued), Q_ARG(QVariantMap, record));
            }
        }
        if (!queued && s_commentsOverview) KMessageBox::error(s_commentsOverview, i18n("The comment could not be added to the durable sync queue."));
    });
    connect(overview, &Thoughts::CommentsOverview::logicalCommentLocationActivated, overview, [](const QString &pdfPath, const QString &commentId, int page, const QVariantMap &location) {
        if (Shell *shell = shellForCommentsOverview(pdfPath)) {
            shell->openTrackedLogicalComment(pdfPath, commentId, page, location);
        }
    });
    connect(overview, &Thoughts::CommentsOverview::agentProposalActivated, overview, [](const QString &pdfPath, const QString &proposalId, const QString &sourcePath, int sourceLine) {
        if (Shell *shell = shellForCommentsOverview(pdfPath)) {
            shell->openTrackedAgentProposal(pdfPath, proposalId, sourcePath, sourceLine);
        } else {
            KMessageBox::error(s_commentsOverview, i18n("No Thoughts PDF window is available for the proposal review."));
        }
    });
    connect(
        overview,
        &Thoughts::CommentsOverview::commentEditRequested,
        overview,
        [](const QString &pdfPath, const QString &annotationId, int page, const QString &contents, const QVariantMap &metadataPatch) {
            if (Shell *shell = shellForCommentsOverview(pdfPath)) {
                shell->editTrackedComment(pdfPath, annotationId, page, contents, metadataPatch);
            }
        },
        Qt::QueuedConnection);
    connect(
        overview,
        &Thoughts::CommentsOverview::commentReplyRequested,
        overview,
        [](const QString &pdfPath, const QString &parentAnnotationId, int page, const QString &contents, const QVariantMap &logicalPayload) {
            Shell *shell = shellForCommentsOverview(pdfPath);
            if (!shell && !logicalPayload.isEmpty()) {
                shell = shellForCommentsOverview();
            }
            if (shell) {
                shell->replyToTrackedComment(pdfPath, parentAnnotationId, page, contents, logicalPayload);
            } else if (s_commentsOverview) {
                s_commentsOverview->completeReply(pdfPath,
                                                  parentAnnotationId,
                                                  {{QStringLiteral("success"), false},
                                                   {QStringLiteral("error"), i18n("No Thoughts PDF window is available to add the reply.")}});
            }
        },
        Qt::QueuedConnection);
    connect(
        overview,
        &Thoughts::CommentsOverview::logicalCommentMutationRequested,
        overview,
        [](const QVariantMap &payload, Thoughts::CommentsOverview::Operation operation, const QString &pdfPath, const QString &commentId) {
            bool queued = false;
            if (Shell *shell = shellForCommentsOverview()) {
                if (KParts::ReadWritePart *part = shell->fedoraGatewayPart()) {
                    QMetaObject::invokeMethod(part, "enqueueLogicalCommentMutation", Qt::DirectConnection, Q_RETURN_ARG(bool, queued), Q_ARG(QVariantMap, payload));
                }
            }
            if (s_commentsOverview) {
                const QVariantMap result {{QStringLiteral("success"), queued}, {QStringLiteral("changed"), queued}, {QStringLiteral("error"), queued ? QString() : i18n("The global comment could not be added to the durable sync queue.")}};
                s_commentsOverview->completeOperation(operation, pdfPath, commentId, result);
            }
        },
        Qt::QueuedConnection);
    connect(
        overview,
        &Thoughts::CommentsOverview::reviewCorrespondenceReviewRequested,
        overview,
        [](const QVariantMap &payload) {
            bool queued = false;
            if (Shell *shell = shellForCommentsOverview()) {
                if (KParts::ReadWritePart *part = shell->fedoraGatewayPart()) {
                    QMetaObject::invokeMethod(part, "enqueueReviewCorrespondenceReview", Qt::DirectConnection, Q_RETURN_ARG(bool, queued), Q_ARG(QVariantMap, payload));
                }
            }
            if (!queued && s_commentsOverview) {
                KMessageBox::error(s_commentsOverview, i18n("The placement decision could not be added to the durable sync queue."));
            }
        },
        Qt::QueuedConnection);
    connect(
        overview,
        &Thoughts::CommentsOverview::agentConversationRemoveRequested,
        overview,
        [](const QVariantMap &record) {
            bool queued = false;
            if (Shell *shell = shellForCommentsOverview()) {
                if (KParts::ReadWritePart *part = shell->fedoraGatewayPart()) {
                    QMetaObject::invokeMethod(part, "enqueueAgentConversation", Qt::DirectConnection, Q_RETURN_ARG(bool, queued), Q_ARG(QVariantMap, record));
                }
            }
            if (!queued && s_commentsOverview) {
                KMessageBox::error(s_commentsOverview, i18n("The conversation could not be removed: the local sync queue is not available."));
            }
        },
        Qt::QueuedConnection);
    connect(
        overview,
        &Thoughts::CommentsOverview::logicalCommentUrlMutationRequested,
        overview,
        [](const QVariantMap &payload, const QString &pdfPath, const QString &commentId) {
            bool queued = false;
            if (Shell *shell = shellForCommentsOverview()) {
                if (KParts::ReadWritePart *part = shell->fedoraGatewayPart()) {
                    QMetaObject::invokeMethod(part, "enqueueLogicalCommentMutation", Qt::DirectConnection, Q_RETURN_ARG(bool, queued), Q_ARG(QVariantMap, payload));
                }
            }
            if (s_commentsOverview) {
                s_commentsOverview->completeChatGptUrlUpdate(
                    pdfPath, commentId, {{QStringLiteral("success"), queued}, {QStringLiteral("changed"), queued}, {QStringLiteral("error"), queued ? QString() : i18n("The global comment link could not be queued.")}});
            }
        },
        Qt::QueuedConnection);
    connect(
        overview,
        &Thoughts::CommentsOverview::commentChatGptUrlUpdateRequested,
        overview,
        [](const QString &pdfPath, const QString &annotationId, const QString &url) {
            QVariantMap result;
            QString error;
            if (Shell *shell = shellForCommentsOverview(pdfPath)) {
                if (KParts::ReadWritePart *part = shell->trackedCommentPart(pdfPath, true, &error)) {
                    const bool invoked = QMetaObject::invokeMethod(part, "updateReviewAnnotationChatGptUrl", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, result), Q_ARG(QString, annotationId), Q_ARG(QString, url));
                    if (!invoked) {
                        error = i18n("Thoughts could not update the ChatGPT link.");
                    }
                }
            } else {
                error = i18n("No Thoughts window is available for the comment update.");
            }
            if (!error.isEmpty()) {
                result = {{QStringLiteral("success"), false}, {QStringLiteral("changed"), false}, {QStringLiteral("error"), error}};
            }
            if (s_commentsOverview) {
                s_commentsOverview->completeChatGptUrlUpdate(pdfPath, annotationId, result);
            }
        },
        Qt::QueuedConnection);
    connect(
        overview,
        &Thoughts::CommentsOverview::commentsRestoreAnchorRequested,
        overview,
        [](const QStringList &commentIds) {
            QVariantMap result;
            bool invoked = false;
            if (Shell *shell = shellForCommentsOverview()) {
                if (KParts::ReadWritePart *part = shell->fedoraGatewayPart()) {
                    invoked = QMetaObject::invokeMethod(part, "restoreCommentAnchors", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, result), Q_ARG(QStringList, commentIds));
                }
            }
            if (!invoked) {
                result = {{QStringLiteral("success"), false}, {QStringLiteral("message"), i18n("No Thoughts window is available to restore the anchor.")}};
            }
            if (s_commentsOverview) {
                s_commentsOverview->completeAnchorRestore(result);
            }
        },
        Qt::QueuedConnection);
    connect(
        overview,
        &Thoughts::CommentsOverview::missingCommentMarkersResolveRequested,
        overview,
        [](const QString &action, const QVariantList &markers) {
            QVariantMap result;
            bool invoked = false;
            if (Shell *shell = shellForCommentsOverview()) {
                if (KParts::ReadWritePart *part = shell->fedoraGatewayPart()) {
                    invoked = QMetaObject::invokeMethod(part, "resolveMissingCommentMarkers", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, result),
                                                        Q_ARG(QString, action), Q_ARG(QVariantList, markers));
                }
            }
            if (!invoked) {
                result = {{QStringLiteral("success"), false}, {QStringLiteral("message"), i18n("No Thoughts window is available to resolve the marker.")}};
            }
            if (s_commentsOverview) {
                s_commentsOverview->completeMissingCommentResolution(result);
            }
        },
        Qt::QueuedConnection);
    // Proposals removed outside Thoughts: Restore Proposal and Dismiss run in the Fedora gateway part.
    const auto removedProposalAction = [](const char *method, void (Thoughts::CommentsOverview::*complete)(const QVariantMap &)) {
        return [method, complete](const QStringList &proposalIds) {
            QVariantMap result;
            bool invoked = false;
            if (Shell *shell = shellForCommentsOverview()) {
                if (KParts::ReadWritePart *part = shell->fedoraGatewayPart()) {
                    invoked = QMetaObject::invokeMethod(part, method, Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, result), Q_ARG(QStringList, proposalIds));
                }
            }
            if (!invoked) {
                result = {{QStringLiteral("success"), false}, {QStringLiteral("message"), i18n("No Thoughts window is available for this proposal.")}};
            }
            if (s_commentsOverview) {
                (s_commentsOverview->*complete)(result);
            }
        };
    };
    connect(overview, &Thoughts::CommentsOverview::proposalsRestoreRequested, overview,
            removedProposalAction("restoreRemovedProposals", &Thoughts::CommentsOverview::completeRemovedProposalRestore), Qt::QueuedConnection);
    connect(overview, &Thoughts::CommentsOverview::proposalsDismissRequested, overview,
            removedProposalAction("dismissRemovedProposals", &Thoughts::CommentsOverview::completeRemovedProposalDismiss), Qt::QueuedConnection);
    connect(overview, &Thoughts::CommentsOverview::commentsDeleteRequested, overview, [](const QList<Thoughts::TrackedCommentTarget> &targets) {
        const QString pdfPath = targets.isEmpty() ? QString() : targets.constFirst().pdfPath;
        if (Shell *shell = shellForCommentsOverview(pdfPath)) {
            shell->deleteTrackedComments(targets);
        }
    });
    connect(overview, &Thoughts::CommentsOverview::commentUndoRequested, overview, [](const QString &pdfPath) {
        if (Shell *shell = shellForCommentsOverview(pdfPath)) {
            shell->undoTrackedCommentChange(pdfPath);
        }
    });
    connect(overview, &Thoughts::CommentsOverview::commentRedoRequested, overview, [](const QString &pdfPath) {
        if (Shell *shell = shellForCommentsOverview(pdfPath)) {
            shell->redoTrackedCommentChange(pdfPath);
        }
    });
    connect(overview, &Thoughts::CommentsOverview::commentsUndoRequested, overview, [](const QList<Thoughts::TrackedCommentTarget> &targets) {
        const QString pdfPath = targets.isEmpty() ? QString() : targets.constFirst().pdfPath;
        if (Shell *shell = shellForCommentsOverview(pdfPath)) {
            shell->undoTrackedCommentChanges(targets);
        }
    });
    connect(overview, &Thoughts::CommentsOverview::commentsRedoRequested, overview, [](const QList<Thoughts::TrackedCommentTarget> &targets) {
        const QString pdfPath = targets.isEmpty() ? QString() : targets.constFirst().pdfPath;
        if (Shell *shell = shellForCommentsOverview(pdfPath)) {
            shell->redoTrackedCommentChanges(targets);
        }
    });
    connect(overview, &Thoughts::CommentsOverview::libraryDisplayNameRequested, overview, [](const QString &filename, const QString &sha256, const QString &displayName) {
        if (Shell *shell = shellForCommentsOverview(QDir(ShellUtils::docsRoot()).filePath(QStringLiteral("current_pdfs/%1").arg(filename)))) {
            shell->renameUnlinkedDocument(filename, sha256, displayName);
        }
    });
    connect(overview, &Thoughts::CommentsOverview::libraryRemovalRequested, overview, [](const QString &pdfPath, const QString &filename, const QString &sha256, const QString &displayName) {
        if (Shell *shell = shellForCommentsOverview(pdfPath)) {
            shell->removePdfFromLibrary(pdfPath, filename, sha256, displayName);
        }
    });
    connect(overview, &Thoughts::CommentsOverview::projectExportRequested, overview, [overview](const QJsonObject &project, const QString &destination) {
        if (Shell *shell = shellForCommentsOverview()) {
            const auto part = shell->m_tabs[shell->m_tabWidget->currentIndex()].part;
            if (!part || !QMetaObject::invokeMethod(part, "exportProjectPdf", Qt::DirectConnection, Q_ARG(QJsonObject, project), Q_ARG(QString, destination)))
                KMessageBox::error(overview, i18n("The project export action is unavailable."));
        }
    });
    connect(overview, &Thoughts::CommentsOverview::projectAttachmentChangedLocally, overview, [](const QString &target) {
        for (KMainWindow *window : KMainWindow::memberList()) {
            if (auto *shell = qobject_cast<Shell *>(window); shell && shell->m_readerDisplayTitles) shell->m_readerDisplayTitles->refresh();
        }
        if (Shell *shell = shellForCommentsOverview()) {
            const QUrl activeUrl = shell->activePdfUrl();
            if (activeUrl.isLocalFile() && QFileInfo(activeUrl.toLocalFile()).fileName() == target) {
                shell->m_pdfLibraryStateReady = false;
                shell->requestPdfLibraryStateRefresh(true);
            }
            shell->scheduleCommentsOverviewDocumentSyncPresentationUpdate();
        }
    });
    connect(overview, &Thoughts::CommentsOverview::trackPdfRequested, overview, []() {
        if (Shell *shell = shellForCommentsOverview()) {
            shell->choosePdfForLibrary();
        }
    });
    return overview;
}

void Shell::updateCommentsOverviewFedoraProjections(const QJsonObject &projections, qint64 frontier)
{
    Q_UNUSED(frontier)
    // Each open tab relays the repository's signal, so one change arrived once per tab and
    // refreshed review stores, the overview and every sidebar that many times.
    m_pendingFedoraProjections = projections;
    if (std::exchange(m_fedoraProjectionsUpdateScheduled, true)) return;
    QTimer::singleShot(0, this, &Shell::applyCommentsOverviewFedoraProjections);
}

void Shell::applyCommentsOverviewFedoraProjections()
{
    m_fedoraProjectionsUpdateScheduled = false;
    const QJsonObject projections = m_pendingFedoraProjections;
    // Agents touch their conversation links while they work: such a change refreshes only
    // the Library rows, not the review cards, their index and every sidebar.
    const bool onlyConversations = !m_appliedFedoraProjections.isEmpty()
        && Thoughts::AgentConversations::onlyConversationChanges(m_appliedFedoraProjections, projections);
    m_appliedFedoraProjections = projections;
    backfillAgentConversations(projections);
    // The review card bar's newest conversation line follows too (it reads them itself).
    if (m_reviewOverview) m_reviewOverview->refreshAgentConversations(projections);
    if (onlyConversations) {
        if (s_commentsOverview) s_commentsOverview->setFedoraProjections(projections);
        return;
    }
    static std::atomic<quint64> reviewIndexGeneration {0};
    const quint64 generation = ++reviewIndexGeneration;
    // The source scanner emits this even when no review window or PDF tab is open.
    // Save current source anchors in the background so Sync can send them promptly.
    QThreadPool::globalInstance()->start([projections, generation] {
        static QMutex reconciliationMutex;
        QMutexLocker lock(&reconciliationMutex);
        if (generation != reviewIndexGeneration.load()) return;
        Thoughts::ReviewStore store(ShellUtils::docsRoot(), [projections] { return projections; });
        store.cards();
        if (generation != reviewIndexGeneration.load()) return;
        const QString indexPath = QDir(Thoughts::Platform::applicationDataRoot())
            .filePath(QStringLiteral("thoughts-server/review-query.sqlite"));
        Thoughts::ReviewStore queryStore(ShellUtils::docsRoot(), [projections] { return projections; }, {}, true);
        QString error;
        if (!queryStore.refreshQueryDatabase(indexPath, &error)) qWarning().noquote() << "Could not refresh review SQL index:" << error;
    });
    if (s_commentsOverview) {
        s_commentsOverview->setFedoraProjections(projections);
    }
    if (m_fedoraManualSyncState == 1) { return; }
    if (m_reviewOverview) {
        m_reviewOverview->reloadFromProjections();
    }
    for (const auto &tab : std::as_const(m_tabs)) {
        refreshSidebarReviewCards(tab.part, projections);
    }
}

void Shell::refreshSidebarReviewCards(KParts::ReadOnlyPart *part, const QJsonObject &projections)
{
    if (!part || !part->url().isLocalFile()) return;
    Thoughts::ReviewStore store(ShellUtils::docsRoot(), [projections] { return projections; });
    QJsonArray cards;
    const QString stem = QFileInfo(part->url().toLocalFile()).completeBaseName();
    // An earlier version opened from History (pdf-versions/<sha256>.pdf) shows the cards
    // retired on it: each stays on the version it was on when it was retired.
    if (Thoughts::PdfVersions::isCachedVersion(part->url().toLocalFile())) {
        for (const auto &card : store.cards()) {
            if (!card.schedule.retired || card.schedule.deleted || card.pdfSha256() != stem) continue;
            QJsonObject record = card.record;
            record.insert(QStringLiteral("sidebar_schedule"), Thoughts::sidebarReviewSchedule(card, false, false));
            cards.append(record);
        }
        QMetaObject::invokeMethod(part, "setSidebarReviewCards", Qt::DirectConnection, Q_ARG(QJsonArray, cards));
        return;
    }
    const QList<Thoughts::ReviewCard> documentCards = store.cards(nullptr, stem);
    // Each card's schedule rides along, read from the cards already folded here. Which of them the
    // review queue takes is activeCards()' rule; the tab compares the cards with its own PDF.
    QSet<QString> active;
    for (const auto &card : store.activeOf(documentCards, nullptr, false)) active.insert(card.id());
    std::optional<QSet<QString>> unpublished;
    for (const auto &card : documentCards) {
        if (card.pdfStem() == stem && !card.schedule.deleted
            && (card.isApproved() || card.status() == QLatin1String("candidate") || card.status() == QLatin1String("needs_reanchor"))) {
            const bool queued = active.contains(card.id());
            bool unpublishedCard = false;
            if (!queued && !card.schedule.retired && !card.schedule.retained) {
                if (!unpublished) unpublished = Thoughts::ProjectConfiguration::unpublishedDocumentIds(ShellUtils::docsRoot());
                unpublishedCard = !unpublished->isEmpty() && unpublished->contains(Thoughts::cardDocumentId(card, ShellUtils::docsRoot()));
            }
            QJsonObject record = card.record;
            QJsonObject schedule = Thoughts::sidebarReviewSchedule(card, queued, unpublishedCard);
            // Why the queue leaves it out, when its passage is the reason (the tab adds "placement").
            const QString link = card.linkingState();
            if (!queued && schedule.value(QStringLiteral("state")).toString() == QLatin1String("unavailable") && !link.isEmpty()
                && link != QLatin1String("linked") && link != QLatin1String("pdf_without_source")
                && !card.sourceLocation.value(QStringLiteral("checking")).toBool())
                schedule.insert(QStringLiteral("not_offered"), QStringLiteral("source"));
            record.insert(QStringLiteral("sidebar_schedule"), schedule);
            cards.append(record);
        }
    }
    QMetaObject::invokeMethod(part, "setSidebarReviewCards", Qt::DirectConnection, Q_ARG(QJsonArray, cards));
    attachDocumentHistory(part);
}

void Shell::attachDocumentHistory(KParts::ReadOnlyPart *part)
{
    if (!part || !part->widget() || !part->url().isLocalFile()) return;
    if (auto *panel = part->widget()->findChild<Thoughts::ReviewFeedbackPanel *>(QStringLiteral("documentHistoryPanel"))) {
        // Kept up to date while shown; else when it is next shown.
        if (panel->isVisible()) updateDocumentHistory(part, panel);
        return;
    }
    // Library PDFs, and their earlier versions (with the History of the document whose records
    // name the version); not a file from elsewhere.
    const QString pdfPath = part->url().toLocalFile();
    QString libraryPdf = pdfPath;
    if (Thoughts::PdfVersions::isCachedVersion(pdfPath)) {
        const QString stem = Thoughts::versionPdfStem(Thoughts::ReviewStore(ShellUtils::docsRoot(), [this] { return effectiveFedoraProjections(); }),
                                                      QFileInfo(pdfPath).completeBaseName());
        libraryPdf = stem.isEmpty() ? QString() : QDir(ShellUtils::docsRoot()).filePath(QStringLiteral("current_pdfs/%1.pdf").arg(stem));
    }
    if (libraryPdf.isEmpty() || Thoughts::DocumentIdentityStore(ShellUtils::docsRoot()).byFilename(QFileInfo(libraryPdf).fileName()).isEmpty()) return;
    auto *panel = new Thoughts::ReviewFeedbackPanel;
    panel->setObjectName(QStringLiteral("documentHistoryPanel"));
    panel->setFrameShape(QFrame::NoFrame);
    panel->setProperty("libraryPdf", libraryPdf);
    QPointer<KParts::ReadOnlyPart> guardedPart(part);
    panel->setRefresh([this, guardedPart, panel] {
        if (guardedPart) updateDocumentHistory(guardedPart, panel);
    });
    // A card of the version this tab shows: its answer, here.
    connect(panel, &Thoughts::ReviewFeedbackPanel::shownPassageRequested, this, [guardedPart](const QVariantList &regions) {
        if (guardedPart) QMetaObject::invokeMethod(guardedPart, "showPassageRegions", Qt::QueuedConnection, Q_ARG(QVariantList, regions));
    });
    connect(panel, &Thoughts::ReviewFeedbackPanel::versionRequested, this, [this, panel, libraryPdf](const QString &sha256, const QDateTime &firstAt, const QVariantList &regions) {
        // The Library's current version: its own tab, its review mask kept.
        if (QFileInfo(libraryPdf).isFile() && Thoughts::FeedbackVersion::cachedFileSha256(libraryPdf) == sha256) {
            QString error;
            KParts::ReadWritePart *part = trackedCommentPart(libraryPdf, true, &error);
            if (!part) {
                KMessageBox::error(this, error.isEmpty() ? i18n("The PDF could not be opened.") : error, i18n("Cannot Open Version"));
                return;
            }
            if (!regions.isEmpty()) QMetaObject::invokeMethod(part, "showPassageRegions", Qt::QueuedConnection, Q_ARG(QVariantList, regions));
            showReviewCardPdfWindow(false);
            return;
        }
        const QString stem = QFileInfo(libraryPdf).completeBaseName();
        const QString documentTitle = m_readerDisplayTitles ? m_readerDisplayTitles->title(QUrl::fromLocalFile(libraryPdf), {}, {}, stem) : stem;
        const QString title = firstAt.isValid() ? i18n("%1 — version of %2", documentTitle, firstAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")))
                                                : i18n("%1 — earlier version", documentTitle);
        if (!m_historyVersionFetcher) m_historyVersionFetcher = new Thoughts::PdfVersions::Fetcher(ShellUtils::docsRoot(), {}, {}, this);
        m_historyVersionFetcher->fetch(sha256, panel, [this, title, regions](const Thoughts::PdfVersions::Result &result) {
            QString error = Thoughts::pdfVersionError(result);
            if (!result.path.isEmpty() && openPdfVersion(result.path, title, 0, regions, false, &error)) return;
            KMessageBox::error(this, error.isEmpty() ? i18n("The PDF could not be opened.") : error, i18n("Cannot Open Version"));
        });
    });
    bool attached = false;
    QMetaObject::invokeMethod(part, "setHistorySidebarPanel", Qt::DirectConnection, Q_RETURN_ARG(bool, attached), Q_ARG(QWidget *, panel));
    if (!attached) delete panel;
}

void Shell::updateDocumentHistory(KParts::ReadOnlyPart *part, Thoughts::ReviewFeedbackPanel *panel)
{
    if (!part || !panel || !part->url().isLocalFile()) return;
    const QString pdfPath = part->url().toLocalFile();
    const QString libraryPdf = panel->property("libraryPdf").toString();
    Thoughts::ReviewStore store(ShellUtils::docsRoot(), [this] { return effectiveFedoraProjections(); });
    QList<Thoughts::ReviewFeedbackPanel::Version> versions = Thoughts::documentHistoryVersions(store, QFileInfo(libraryPdf).completeBaseName(), libraryPdf);
    // Every version, also one without cards: the current one, those still here and those with comments.
    const QString documentId = Thoughts::DocumentIdentityStore(ShellUtils::docsRoot()).forFile(libraryPdf).value(QStringLiteral("document_id")).toString();
    Thoughts::addDocumentVersions(versions,
                                  documentId.isEmpty() ? QList<QJsonObject>() : Thoughts::DocumentFeedback::recordsOfDocument(effectiveFedoraProjections(), documentId),
                                  Thoughts::keptDocumentVersions(ShellUtils::docsRoot(), QFileInfo(libraryPdf).fileName()),
                                  QFileInfo(libraryPdf).isFile() ? Thoughts::FeedbackVersion::cachedFileSha256(libraryPdf) : QString(), libraryPdf);
    // The version of this tab: the Library's, else the earlier one it holds (named by its SHA-256).
    const QString shownSha256 = pdfPath == libraryPdf ? QString() : QFileInfo(pdfPath).completeBaseName();
    for (auto &version : versions) version.shown = shownSha256.isEmpty() ? version.current : version.sha256 == shownSha256;
    QSet<QString> cards;
    int feedback = 0;
    for (const auto &version : versions) {
        for (const auto &entry : version.entries) {
            cards.insert(entry.cardId);
            if (!entry.feedback.isEmpty() && !entry.feedback.value(QStringLiteral("deleted")).toBool() && !entry.feedback.value(QStringLiteral("carried")).toBool()) ++feedback;
        }
    }
    int comments = 0;
    for (const auto &version : versions) comments += version.comments.size();
    QStringList summaryParts;
    const qsizetype recorded = std::count_if(versions.cbegin(), versions.cend(), [](const auto &version) { return !version.sha256.isEmpty(); });
    if (recorded) summaryParts.append(i18np("1 version", "%1 versions", recorded));
    if (comments) summaryParts.append(i18np("1 comment", "%1 comments", comments));
    if (!cards.isEmpty()) summaryParts.append(i18np("1 card", "%1 cards", cards.size()) + QStringLiteral(" · ") + i18np("1 feedback", "%1 feedback", feedback));
    const QString summary = summaryParts.isEmpty() ? i18n("Nothing recorded on this document yet.") : summaryParts.join(QStringLiteral(" · "));
    panel->setMaskedCard(m_reviewOverview ? m_reviewOverview->maskedHistoryCard() : QString());
    panel->setHistory(versions, summary);
}

void Shell::showCardHistory(const QString &cardId)
{
    const int activeTab = m_tabWidget ? m_tabWidget->currentIndex() : -1;
    KParts::ReadOnlyPart *part = activeTab >= 0 && activeTab < m_tabs.size() ? m_tabs.at(activeTab).part : nullptr;
    if (!part || !part->widget()) return;
    attachDocumentHistory(part);
    auto *panel = part->widget()->findChild<Thoughts::ReviewFeedbackPanel *>(QStringLiteral("documentHistoryPanel"));
    bool shown = false;
    if (panel) QMetaObject::invokeMethod(part, "showHistorySidebar", Qt::DirectConnection, Q_RETURN_ARG(bool, shown));
    if (!shown) {
        KMessageBox::information(this, i18n("This document has no History: it is not a Library PDF."), i18n("History"));
        return;
    }
    // Shown, it has refreshed itself; else bring it up to date now.
    updateDocumentHistory(part, panel);
    panel->scrollToCard(cardId);
}

void Shell::addOutlineDocumentActions(QMenu *menu, const QString &pdfPath, int page, const QString &title)
{
    commentsOverview()->addOutlineDocumentActions(menu, pdfPath, page, title);
}

void Shell::updateCommentsOverviewPendingChanges(const QJsonObject &snapshot)
{
    m_globalPendingChangesSnapshot = snapshot;
    // Relayed once per open tab; present the latest snapshot once per burst.
    if (std::exchange(m_pendingChangesUpdateScheduled, true)) return;
    QTimer::singleShot(0, this, &Shell::applyCommentsOverviewPendingChanges);
}

void Shell::applyCommentsOverviewPendingChanges()
{
    m_pendingChangesUpdateScheduled = false;
    // More waits to be sent than before, and no Sync runs: the user changed
    // something on this computer, which ends the wait of the automatic Syncs.
    static int knownPendingTotal = -1;
    const int pendingTotal = m_globalPendingChangesSnapshot.value(QStringLiteral("available")).toBool() ? m_globalPendingChangesSnapshot.value(QStringLiteral("total")).toInt() : -1;
    if (pendingTotal > knownPendingTotal && knownPendingTotal >= 0 && m_fedoraManualSyncState != 1 && s_automaticSync.notBeforeMs() > 0) {
        s_automaticSync.localChange();
        refreshAutomaticSyncNotice();
    }
    if (pendingTotal >= 0) {
        knownPendingTotal = pendingTotal;
    }
    updatePdfSyncStatus();
    updateCommentsOverviewDocumentSyncPresentations();
}

QJsonObject Shell::handleSyncCommandRequest(const QJsonObject &request)
{
    if (request.value(QStringLiteral("schema_version")).toInt() != 1) {
        return Thoughts::syncCommandError(QStringLiteral("invalid_request"), QStringLiteral("Unsupported command schema version."));
    }
    const QString command = request.value(QStringLiteral("command")).toString();
    if (command == QLatin1String("merge-review-cards") && m_fedoraManualSyncState == 1) {
        // A merge writes the source and queues its cards; a running Sync would read them halfway.
        return Thoughts::syncCommandError(QStringLiteral("busy"), QStringLiteral("A Sync is running. Retry the identical request when it has finished."));
    }
    if (command == QLatin1String("create-review-card") || command == QLatin1String("list-review-cards") || command == QLatin1String("delete-review-card") || command == QLatin1String("set-review-card-interval")
        || command == QLatin1String("move-review-card") || command == QLatin1String("merge-review-cards")) {
        Thoughts::ReviewStore store(ShellUtils::docsRoot(), [this] { return effectiveFedoraProjections(); }, {},
            command == QLatin1String("list-review-cards"));
        const QString dataRoot = Thoughts::Platform::applicationDataRoot();
        const auto result = store.sourceCardCommand(request, QDir(dataRoot).filePath(QStringLiteral("thoughts-server/source-bundle")));
        if (command != QLatin1String("list-review-cards") && result.value(QStringLiteral("accepted")).toBool()) {
            if (m_reviewOverview) m_reviewOverview->reload();
            if (s_commentsOverview) s_commentsOverview->reloadReviewCards();
            QString error;
            store.refreshQueryDatabase(QDir(dataRoot).filePath(QStringLiteral("thoughts-server/review-query.sqlite")), &error);
            if (!error.isEmpty()) qWarning().noquote() << "Could not refresh review SQL index:" << error;
        }
        return result;
    }
    if (command == QLatin1String("review-tags-apply")) {
        // thoughtsctl review-tags apply: the same merges and layout as Compact tags and the tag picker.
        Thoughts::ReviewTagPlan::Plan plan;
        QString error;
        if (!Thoughts::ReviewTagPlan::parse(request.value(QStringLiteral("plan")).toObject(), &plan, &error))
            return Thoughts::syncCommandError(QStringLiteral("invalid_plan"), error);
        Thoughts::ReviewStore store(ShellUtils::docsRoot(), [this] { return effectiveFedoraProjections(); });
        const QJsonObject result = Thoughts::ReviewTagPlan::apply(plan, store);
        const QJsonObject applied = result.value(QStringLiteral("applied")).toObject();
        if (applied.value(QStringLiteral("merges_recorded")).toInt() > 0 || applied.value(QStringLiteral("groups_saved")).toBool()
            || result.value(QStringLiteral("error_code")).toString() == QLatin1String("write_failed")) {
            if (m_reviewOverview) m_reviewOverview->reload();
            if (s_commentsOverview) s_commentsOverview->reloadReviewCards();
            const QString dataRoot = Thoughts::Platform::applicationDataRoot();
            store.refreshQueryDatabase(QDir(dataRoot).filePath(QStringLiteral("thoughts-server/review-query.sqlite")), &error);
            if (!error.isEmpty()) qWarning().noquote() << "Could not refresh review SQL index:" << error;
        }
        return result;
    }
    if (command == QLatin1String("delete-comment")) {
        return deleteCommentCommand(request);
    }
    if (command == QLatin1String("resolve-source-conflict")) {
        return resolveSourceConflictCommand(request);
    }
    if (command == QLatin1String("list-todos") || command == QLatin1String("add-todo") || command == QLatin1String("edit-todo")) {
        return todoCommand(command, request);
    }
    if (command == QLatin1String("link-conversation")) {
        return linkConversationCommand(request);
    }
    if (command == QLatin1String("create-project") || command == QLatin1String("import-document") || command == QLatin1String("remove-project")) {
        return libraryProjectCommand(request);
    }
    if (command == QLatin1String("open-agent-notes")) {
        return openAgentNotesCommand(request);
    }
    if (command == QLatin1String("decide-proposal")) {
        // The same local decision as Accept/Reject in the Comments view, with its claim.
        const QJsonObject result = Thoughts::ProposalDecisions::decideProposal(ShellUtils::docsRoot(), request);
        if (result.value(QStringLiteral("accepted")).toBool() && !result.value(QStringLiteral("replayed")).toBool()) {
            if (s_commentsOverview) {
                s_commentsOverview->reload();
            }
            proposalDecided();
        }
        return result;
    }
    if (command == QLatin1String("activate")) {
        // Another launch of Thoughts handed over its command line (Windows
        // starts a new process for every file, link or Start menu click).
        // Replay it like the file-open events macOS delivers.
        for (const QJsonValue &value : request.value(QStringLiteral("arguments")).toArray()) {
            const QString argument = value.toString();
            const QUrl url = argument.startsWith(QLatin1String("thoughts:"), Qt::CaseInsensitive) ? QUrl(argument) : QUrl::fromLocalFile(argument);
            if (argument.isEmpty() || !url.isValid()) {
                continue;
            }
            QFileOpenEvent event(url);
            QCoreApplication::sendEvent(qApp, &event);
        }
        if (request.value(QStringLiteral("arguments")).toArray().isEmpty()) {
            showNormal();
            raise();
            activateWindow();
        }
        return {{QStringLiteral("schema_version"), 1}, {QStringLiteral("accepted"), true}, {QStringLiteral("terminal"), true}, {QStringLiteral("state"), QStringLiteral("activated")}};
    }
    if (command == QLatin1String("open")) {
        const QString document = request.value(QStringLiteral("document")).toString();
        const QJsonValue pageValue = request.value(QStringLiteral("page"));
        const int page = pageValue.toInt(-1);
        if (document.isEmpty() || !pageValue.isDouble() || page < 1 || pageValue.toDouble() != page
            || (!QDir::isAbsolutePath(document) && !Thoughts::validSyncCommandTarget(document))) {
            return Thoughts::syncCommandError(QStringLiteral("invalid_request"), QStringLiteral("Provide an absolute PDF path or exact Library filename and a positive integer page."));
        }
        const QFileInfo info(QDir::isAbsolutePath(document) ? document
            : QDir(ShellUtils::docsRoot()).filePath(QStringLiteral("current_pdfs/") + document));
        if (!info.isFile() || !info.isReadable() || info.suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) != 0) {
            return Thoughts::syncCommandError(QStringLiteral("invalid_request"), QStringLiteral("The requested PDF does not exist or is not readable."));
        }
        // Keep the stable Library path: current_pdfs may point to a replaceable publication stage.
        const QString path = info.absoluteFilePath();
        Shell *destination = shellForCommentsOverview(path);
        if (!destination) {
            destination = this;
        }
        QString error;
        KParts::ReadWritePart *part = destination->trackedCommentPart(path, true, &error);
        if (!part) {
            return Thoughts::syncCommandError(QStringLiteral("open_failed"), error);
        }
        uint pages = 0;
        if (!QMetaObject::invokeMethod(part, "pages", Qt::DirectConnection, Q_RETURN_ARG(uint, pages)) || !pages) {
            return Thoughts::syncCommandError(QStringLiteral("open_failed"), QStringLiteral("The PDF did not load."));
        }
        if (uint(page) > pages) {
            return Thoughts::syncCommandError(QStringLiteral("invalid_request"), QStringLiteral("Page must be between 1 and %1.").arg(pages));
        }
        uint currentPage = 0;
        if (!QMetaObject::invokeMethod(part, "goToPage", Qt::DirectConnection, Q_ARG(uint, uint(page)))
            || !QMetaObject::invokeMethod(part, "currentPage", Qt::DirectConnection, Q_RETURN_ARG(uint, currentPage))
            || currentPage != uint(page)) {
            return Thoughts::syncCommandError(QStringLiteral("open_failed"), QStringLiteral("Thoughts could not navigate to the requested page."));
        }
        destination->showNormal();
        destination->raise();
        destination->activateWindow();
        return {{QStringLiteral("schema_version"), 1}, {QStringLiteral("accepted"), true},
                {QStringLiteral("terminal"), true}, {QStringLiteral("state"), QStringLiteral("opened")},
                {QStringLiteral("document"), path}, {QStringLiteral("page"), int(currentPage)},
                {QStringLiteral("page_count"), int(pages)}};
    }
    if (command == QLatin1String("sync")) {
        return startSyncCommand(request.value(QStringLiteral("target")).toString());
    }
    if (command == QLatin1String("status")) {
        return syncCommandStatus(request.value(QStringLiteral("operation_id")).toString());
    }
    return Thoughts::syncCommandError(QStringLiteral("invalid_request"), QStringLiteral("Unknown Thoughts command."));
}

namespace
{
// Live markers of one comment in a source, as Thoughts writes them: the TeX
// commands outside %-comments, or the Markdown metadata line outside code.
int commentMarkerCount(const QString &text, const QString &id, bool markdown)
{
    int count = 0;
    const QStringList lines = text.split(QLatin1Char('\n'));
    if (markdown) {
        const QRegularExpression meta(QStringLiteral("^<!--\\s*thoughts:v1:meta\\s+\\{.*\"key\"\\s*:\\s*\"comment:%1:").arg(QRegularExpression::escape(id)));
        QChar fence;
        int fenceLength = 0;
        for (const QString &raw : lines) {
            const QString line = raw.trimmed();
            if (line.startsWith(QLatin1String("```")) || line.startsWith(QLatin1String("~~~"))) {
                int length = 0;
                while (length < line.size() && line.at(length) == line.at(0)) ++length;
                if (fence.isNull()) {
                    fence = line.at(0);
                    fenceLength = length;
                } else if (fence == line.at(0) && length >= fenceLength && line.mid(length).trimmed().isEmpty()) {
                    fence = QChar();
                }
                continue;
            }
            if (fence.isNull() && !raw.contains(QLatin1Char('`')) && !raw.startsWith(QLatin1String("    ")) && !raw.startsWith(QLatin1Char('\t'))
                && meta.match(line).hasMatch()) {
                ++count;
            }
        }
        return count;
    }
    const QRegularExpression marker(
        QStringLiteral("\\\\(?:CommentAnchor|CommentHighlight|CommentStrikethrough|ReviewAnchor)\\s*\\{%1\\}").arg(QRegularExpression::escape(id)));
    for (const QString &line : lines) {
        auto matches = marker.globalMatch(line);
        while (matches.hasNext()) {
            const auto match = matches.next();
            bool commented = false;
            for (int index = 0; index < match.capturedStart() && !commented; ++index) {
                if (line.at(index) == QLatin1Char('\\')) ++index;
                else commented = line.at(index) == QLatin1Char('%');
            }
            if (!commented) ++count;
        }
    }
    return count;
}

QString sourceSha256(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
}

// Deletes one comment by the ID in its source marker, through the same
// explicit Delete as the comment UI: it is recorded as user_deleted, its
// marker leaves the source, and the next Sync sends the deletion.
QJsonObject Shell::libraryProjectCommand(const QJsonObject &request)
{
    // The same Library changes as Add project and Import document: Sync then
    // creates the Project on Fedora and uploads each imported PDF.
    const auto invalid = [](const QString &message) { return Thoughts::syncCommandError(QStringLiteral("invalid_request"), message); };
    const auto failed = [](const QString &message) { return Thoughts::syncCommandError(QStringLiteral("library_change_failed"), message); };
    const auto done = [](QJsonObject result) {
        if (s_commentsOverview) s_commentsOverview->libraryProjectsChangedByCommand();
        result.insert(QStringLiteral("schema_version"), 1);
        result.insert(QStringLiteral("accepted"), true);
        result.insert(QStringLiteral("terminal"), true);
        return result;
    };
    const auto validName = [](const QString &name) {
        return !name.isEmpty() && name.size() <= 200 && std::none_of(name.cbegin(), name.cend(), [](QChar c) { return c.unicode() < 32 || c.unicode() == 127; });
    };
    Thoughts::LibraryProjectStore store(ShellUtils::docsRoot());
    const QJsonArray projects = store.projects(false);
    if (!store.error().isEmpty()) return failed(store.error());

    if (request.value(QStringLiteral("command")).toString() == QLatin1String("create-project")) {
        const QString name = request.value(QStringLiteral("name")).toString().trimmed();
        const QUuid requestId = QUuid::fromString(request.value(QStringLiteral("request_id")).toString());
        if (!validName(name)) return invalid(QStringLiteral("name must have 1 to 200 characters without line breaks."));
        if (requestId.isNull()) return invalid(QStringLiteral("request_id must be a UUID."));
        // The request ID names the Project, so a retried request finds the Project it created.
        const QString id = QStringLiteral("new:") + requestId.toString(QUuid::WithoutBraces);
        for (const auto &value : projects) {
            const auto project = value.toObject();
            if (project.value(QStringLiteral("id")).toString() == id)
                return done({{QStringLiteral("state"), QStringLiteral("created")}, {QStringLiteral("project_id"), id},
                             {QStringLiteral("project"), project.value(QStringLiteral("name"))}, {QStringLiteral("replayed"), true}});
            if (project.value(QStringLiteral("name")).toString().compare(name, Qt::CaseInsensitive) == 0)
                return Thoughts::syncCommandError(QStringLiteral("project_exists"), QStringLiteral("A project named \"%1\" already exists.").arg(name));
        }
        const QString error = store.enqueue(QStringLiteral("create"), {{QStringLiteral("id"), id}, {QStringLiteral("revision"), 0}}, {{QStringLiteral("name"), name}});
        if (!error.isEmpty()) return failed(error);
        return done({{QStringLiteral("state"), QStringLiteral("created")}, {QStringLiteral("project_id"), id}, {QStringLiteral("project"), name}});
    }

    // The other commands name a Project by its ID or name.
    const QString wanted = request.value(QStringLiteral("project")).toString();
    QJsonObject project;
    for (const auto &value : projects) {
        const auto candidate = value.toObject();
        if (candidate.value(QStringLiteral("id")).toString() == wanted || candidate.value(QStringLiteral("name")).toString().compare(wanted, Qt::CaseInsensitive) == 0) {
            project = candidate; break;
        }
    }
    if (project.isEmpty()) return Thoughts::syncCommandError(QStringLiteral("project_not_found"), QStringLiteral("No Library project is named \"%1\".").arg(wanted));
    if (request.value(QStringLiteral("command")).toString() == QLatin1String("remove-project")) {
        // Remove project in the Library (thoughtsctl only; agents get no MCP tool for it).
        const QString error = store.enqueue(QStringLiteral("remove"), project);
        if (!error.isEmpty()) return failed(error);
        return done({{QStringLiteral("state"), QStringLiteral("removed")}, {QStringLiteral("project_id"), project.value(QStringLiteral("id"))},
                     {QStringLiteral("project"), project.value(QStringLiteral("name"))}});
    }

    // import-document: a local PDF into the Project.
    const QString path = request.value(QStringLiteral("path")).toString();
    if (!QFileInfo(path).isAbsolute() || !QFileInfo(path).isFile()) return invalid(QStringLiteral("path must be an existing local PDF file."));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.read(5) != QByteArrayLiteral("%PDF-")) return invalid(QStringLiteral("path is not a readable PDF."));
    file.seek(0);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return invalid(QStringLiteral("path could not be read."));
    file.close();
    const QString marker = QLatin1Char('_') + QString::fromLatin1(hash.result().toHex().left(12)) + QLatin1Char('_');
    const auto imported = [&marker](const QJsonObject &project) {
        for (const auto &value : project.value(QStringLiteral("documents")).toArray()) {
            const auto document = value.toObject();
            if (document.value(QStringLiteral("attachment_filename")).toString(document.value(QStringLiteral("filename")).toString()).contains(marker)) return document;
        }
        return QJsonObject();
    };
    const auto result = [&project](const QJsonObject &document, bool replayed) {
        return QJsonObject{{QStringLiteral("state"), QStringLiteral("imported")}, {QStringLiteral("project_id"), project.value(QStringLiteral("id"))},
                           {QStringLiteral("project"), project.value(QStringLiteral("name"))}, {QStringLiteral("replayed"), replayed},
                           {QStringLiteral("document_id"), document.value(QStringLiteral("id"))}, {QStringLiteral("name"), document.value(QStringLiteral("name"))},
                           {QStringLiteral("filename"), document.value(QStringLiteral("attachment_filename")).toString(document.value(QStringLiteral("filename")).toString())}};
    };
    if (const auto existing = imported(project); !existing.isEmpty()) return done(result(existing, true));
    const QString name = request.value(QStringLiteral("name")).toString(QFileInfo(path).completeBaseName()).trimmed().left(150);
    if (!validName(name)) return invalid(QStringLiteral("name must have 1 to 150 characters without line breaks."));
    const QUrl link(request.value(QStringLiteral("link")).toString());
    if (request.contains(QStringLiteral("link")) && (!link.isValid() || (link.scheme() != QLatin1String("https") && link.scheme() != QLatin1String("http"))))
        return invalid(QStringLiteral("link must be an http or https URL."));
    const QString error = store.importPdf(project, path, name, link);
    if (!error.isEmpty()) return failed(error);
    for (const auto &value : store.projects(false)) {
        if (value.toObject().value(QStringLiteral("id")) != project.value(QStringLiteral("id"))) continue;
        if (const auto document = imported(value.toObject()); !document.isEmpty()) return done(result(document, false));
    }
    return done(result({{QStringLiteral("name"), name}}, false));
}

QJsonObject Shell::linkConversationCommand(const QJsonObject &request)
{
    namespace Conversations = Thoughts::AgentConversations;
    const auto invalid = [](const QString &message) { return Thoughts::syncCommandError(QStringLiteral("invalid_request"), message); };
    const QString agent = request.value(QStringLiteral("agent")).toString();
    const QString conversationId = request.value(QStringLiteral("conversation_id")).toString();
    const QString url = request.value(QStringLiteral("conversation_url")).toString();
    const QString linkedBy = request.value(QStringLiteral("linked_by")).toString(QStringLiteral("tool"));
    const QString titleSource = request.value(QStringLiteral("title_source")).toString(QStringLiteral("fallback"));
    const QString tool = request.value(QStringLiteral("tool")).toString();
    // How a review card entry is first created: the prompt hook, a Thoughts tool, or the agent.
    const QString cardLinkedBy = request.value(QStringLiteral("card_linked_by")).toString(linkedBy == QLatin1String("agent") ? QStringLiteral("agent") : QStringLiteral("tool"));
    static const QRegularExpression toolPattern(QStringLiteral("^[a-z_]{0,64}$"));
    if (agent != QLatin1String("claude") && agent != QLatin1String("codex")) return invalid(QStringLiteral("agent must be claude or codex."));
    if (!Conversations::validConversationId(conversationId)) return invalid(QStringLiteral("conversation_id must be a lowercase UUID."));
    if (!Conversations::validConversationUrl(agent, conversationId, url)) return invalid(QStringLiteral("conversation_url is not a link to this conversation."));
    if (linkedBy != QLatin1String("agent") && linkedBy != QLatin1String("tool")) return invalid(QStringLiteral("linked_by must be agent or tool."));
    if (Conversations::titleRank(titleSource) < 0) return invalid(QStringLiteral("title_source must be agent, app, prompt or fallback."));
    if (!toolPattern.match(tool).hasMatch()) return invalid(QStringLiteral("tool must be an MCP tool name."));
    if (cardLinkedBy != QLatin1String("prompt") && cardLinkedBy != QLatin1String("tool") && cardLinkedBy != QLatin1String("agent"))
        return invalid(QStringLiteral("card_linked_by must be prompt, tool or agent."));
    for (const auto &field : {QStringLiteral("title"), QStringLiteral("session_name"), QStringLiteral("conversation_url"), QStringLiteral("tool"), QStringLiteral("card_linked_by")}) {
        if (request.contains(field) && !request.value(field).isString()) return invalid(QStringLiteral("%1 must be a string.").arg(field));
    }

    // The target: exactly one of project, source, document or review_card_id.
    QJsonObject target;
    int targets = 0;
    for (const auto &field : {QStringLiteral("project"), QStringLiteral("source"), QStringLiteral("document"), QStringLiteral("review_card_id")}) {
        if (!request.contains(field)) continue;
        ++targets;
        if (!request.value(field).isString() || request.value(field).toString().isEmpty()) return invalid(QStringLiteral("%1 must be a non-empty string.").arg(field));
        target.insert(field, request.value(field));
    }
    if (targets != 1) return invalid(QStringLiteral("Give exactly one of project, source, document or review_card_id."));
    const QString docsRoot = ShellUtils::docsRoot();
    const QJsonObject projections = effectiveFedoraProjections();
    QString cardId;
    if (target.contains(QStringLiteral("review_card_id"))) {
        const QString requested = target.value(QStringLiteral("review_card_id")).toString();
        QString stem;
        Thoughts::ReviewStore store(docsRoot, [projections] { return projections; });
        for (const auto &card : store.cards()) {
            if (card.id().compare(requested, Qt::CaseInsensitive) == 0) {
                stem = card.pdfStem();
                cardId = card.id().toLower();
                break;
            }
        }
        if (stem.isEmpty()) return Thoughts::syncCommandError(QStringLiteral("card_not_found"), QStringLiteral("No review card has this ID."));
        // A card id that is not a UUID links the Project only (review_cards holds UUIDs).
        if (!Conversations::validCardId(cardId)) cardId.clear();
        target = QJsonObject{{QStringLiteral("document"), QString(stem + QStringLiteral(".pdf"))}};
    }
    const auto resolution = Conversations::resolveProject(Thoughts::LibraryProjectStore(docsRoot).projects(false), docsRoot, target);
    if (resolution.project.isEmpty()) return Thoughts::syncCommandError(resolution.errorCode, resolution.message);
    const QString projectId = resolution.project.value(QStringLiteral("id")).toString();

    Conversations::Touch touch;
    touch.projectId = projectId;
    touch.agent = agent;
    touch.conversationId = conversationId;
    touch.conversationUrl = url;
    touch.title = request.value(QStringLiteral("title")).toString();
    touch.titleSource = titleSource;
    touch.sessionName = request.value(QStringLiteral("session_name")).toString();
    touch.linkedBy = linkedBy;
    touch.tool = tool;
    touch.reviewCardId = cardId;
    touch.cardLinkedBy = cardLinkedBy;
    const QString linkId = Conversations::linkId(projectId, agent, conversationId);
    const auto result = Conversations::touch(projections.value(Conversations::projectionKey(linkId)).toObject(), touch, Thoughts::Platform::desktopOriginId(),
                                             QDateTime::currentDateTimeUtc(), QSysInfo::machineHostName());
    if (result.written) {
        bool queued = false;
        if (KParts::ReadWritePart *part = fedoraGatewayPart())
            QMetaObject::invokeMethod(part, "enqueueAgentConversation", Qt::DirectConnection, Q_RETURN_ARG(bool, queued), Q_ARG(QVariantMap, result.record.toVariantMap()));
        if (!queued) return Thoughts::syncCommandError(QStringLiteral("queue_unavailable"), QStringLiteral("The conversation could not be added to the local sync queue."));
    }
    return {{QStringLiteral("schema_version"), 1},
            {QStringLiteral("accepted"), true},
            {QStringLiteral("terminal"), true},
            {QStringLiteral("link_id"), linkId},
            {QStringLiteral("project_id"), projectId},
            {QStringLiteral("project_name"), resolution.project.value(QStringLiteral("name")).toString()},
            {QStringLiteral("written"), result.written},
            {QStringLiteral("removed"), result.removed},
            {QStringLiteral("title"), result.record.value(QStringLiteral("title")).toString()},
            {QStringLiteral("conversation_url"), result.record.value(QStringLiteral("conversation_url")).toString()},
            {QStringLiteral("review_card_id"), cardId.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(cardId)},
            {QStringLiteral("card_written"), result.written && result.cardWritten}};
}

void Shell::backfillAgentConversations(const QJsonObject &projections)
{
    namespace Conversations = Thoughts::AgentConversations;
    if (m_agentConversationBackfillRunning) return;
    KConfigGroup group(KSharedConfig::openConfig(), QStringLiteral("AgentConversations"));
    if (group.readEntry("ProposalBackfillV1Done", false)) return;
    bool hasProposals = false;
    for (auto it = projections.constBegin(); it != projections.constEnd() && !hasProposals; ++it) hasProposals = it.key().startsWith(QLatin1String("proposal\nproposals\n"));
    KParts::ReadWritePart *part = fedoraGatewayPart();
    if (!hasProposals || !part) return;
    m_agentConversationBackfillRunning = true;
    const QString docsRoot = ShellUtils::docsRoot();
    Conversations::BackfillLookup lookup;
    lookup.link = [](const QString &stored, const QString &id) { return Thoughts::proposalConversationLink(stored, id); };
    lookup.codexHome = qEnvironmentVariableIsEmpty("CODEX_HOME") ? QDir::home().filePath(QStringLiteral(".codex")) : qEnvironmentVariable("CODEX_HOME");
    lookup.claudeHome = qEnvironmentVariableIsEmpty("CLAUDE_CONFIG_DIR") ? QDir::home().filePath(QStringLiteral(".claude")) : qEnvironmentVariable("CLAUDE_CONFIG_DIR");
    bool complete = true;
    QJsonObject written;
    for (const auto &touch : Conversations::proposalBackfill(projections, Thoughts::LibraryProjectStore(docsRoot).projects(false), docsRoot, lookup)) {
        const QString key = Conversations::projectionKey(Conversations::linkId(touch.projectId, touch.agent, touch.conversationId));
        const auto result = Conversations::touch(written.contains(key) ? written.value(key).toObject() : projections.value(key).toObject(), touch,
                                                 Thoughts::Platform::desktopOriginId(), QDateTime::currentDateTimeUtc(), QSysInfo::machineHostName());
        if (!result.written) continue;
        bool queued = false;
        QMetaObject::invokeMethod(part, "enqueueAgentConversation", Qt::DirectConnection, Q_RETURN_ARG(bool, queued), Q_ARG(QVariantMap, result.record.toVariantMap()));
        if (queued) written.insert(key, result.record);
        else complete = false;
    }
    if (complete) {
        group.writeEntry("ProposalBackfillV1Done", true);
        group.sync();
    }
    m_agentConversationBackfillRunning = false;
}

QJsonObject Shell::deleteCommentCommand(const QJsonObject &request)
{
    const QSet<QString> allowed {QStringLiteral("schema_version"), QStringLiteral("command"), QStringLiteral("comment_id"),
                                 QStringLiteral("source"), QStringLiteral("expected_source_sha256")};
    for (auto it = request.begin(); it != request.end(); ++it) {
        if (!allowed.contains(it.key())) {
            return Thoughts::syncCommandError(QStringLiteral("invalid_request"), QStringLiteral("Unexpected field: %1").arg(it.key()));
        }
    }
    static const QRegularExpression idPattern(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    const QString id = request.value(QStringLiteral("comment_id")).toString();
    if (!idPattern.match(id).hasMatch()) {
        return Thoughts::syncCommandError(QStringLiteral("invalid_request"), QStringLiteral("Provide the comment ID from its source marker."));
    }
    const bool checkSource = request.contains(QStringLiteral("source")) || request.contains(QStringLiteral("expected_source_sha256"));

    // The marker carries the anchor ID, which is normally the comment's own ID.
    const QString prefix = QStringLiteral("logical_comment\ncomments\n");
    const QJsonObject projections = effectiveFedoraProjections();
    QJsonObject comment = projections.value(prefix + id).toObject();
    if (comment.isEmpty()) {
        for (auto it = projections.begin(); it != projections.end(); ++it) {
            if (!it.key().startsWith(prefix)) continue;
            const QJsonObject candidate = it.value().toObject();
            if (candidate.value(QStringLiteral("scope")).toObject().value(QStringLiteral("anchor_id")).toString() != id) continue;
            if (!comment.isEmpty()) {
                return Thoughts::syncCommandError(QStringLiteral("comment_unavailable"), QStringLiteral("More than one Thoughts comment uses this marker ID."));
            }
            comment = candidate;
        }
    }
    const QString commentId = comment.value(QStringLiteral("comment_id")).toString();
    if (comment.isEmpty() || commentId.isEmpty()) {
        return Thoughts::syncCommandError(QStringLiteral("comment_unavailable"), QStringLiteral("Thoughts has no comment with this ID. Sync, then try again."));
    }
    // A comment deleted through another copy of its marker (the same section in
    // several projects) leaves this marker behind: with a source, only remove it.
    const bool alreadyDeleted = comment.value(QStringLiteral("deleted")).toBool();
    if (alreadyDeleted && !checkSource) {
        return Thoughts::syncCommandError(QStringLiteral("comment_unavailable"), QStringLiteral("This comment is already deleted in Thoughts."));
    }
    const QString anchorId = comment.value(QStringLiteral("scope")).toObject().value(QStringLiteral("anchor_id")).toString(commentId);

    QString sourcePath;
    QString relative;
    bool markdown = false;
    if (checkSource) {
        const QString root = QFileInfo(ShellUtils::docsRoot()).canonicalFilePath();
        const QString requested = request.value(QStringLiteral("source")).toString();
        sourcePath = requested.isEmpty() ? QString() : QFileInfo(QDir(root).absoluteFilePath(requested)).canonicalFilePath();
        relative = QDir(root).relativeFilePath(sourcePath);
        const QString suffix = QFileInfo(sourcePath).suffix().toLower();
        markdown = suffix == QLatin1String("md") || suffix == QLatin1String("markdown");
        const QString expected = request.value(QStringLiteral("expected_source_sha256")).toString();
        static const QRegularExpression sha256Pattern(QStringLiteral("^[0-9a-f]{64}$"));
        QFile source(sourcePath);
        if (root.isEmpty() || sourcePath.isEmpty() || !sourcePath.startsWith(root + QLatin1Char('/')) || (!markdown && suffix != QLatin1String("tex"))
            || !sha256Pattern.match(expected).hasMatch() || !source.open(QIODevice::ReadOnly)) {
            return Thoughts::syncCommandError(QStringLiteral("invalid_source"), QStringLiteral("Choose a saved .tex or Markdown source in the Thoughts Library with its current SHA-256."));
        }
        const QByteArray bytes = source.readAll();
        if (sourceSha256(bytes) != expected) {
            return Thoughts::syncCommandError(QStringLiteral("source_conflict"), QStringLiteral("The source changed since it was read. Reload it and try again."));
        }
        const int markers = commentMarkerCount(QString::fromUtf8(bytes), anchorId, markdown);
        if (markers != 1) {
            return Thoughts::syncCommandError(QStringLiteral("invalid_source"),
                markers ? QStringLiteral("This source contains the comment's marker %1 times; keep exactly one, then try again.").arg(markers)
                        : QStringLiteral("This source does not contain the comment's marker."));
        }
        const QString linkedFile = comment.value(QStringLiteral("scope")).toObject().value(QStringLiteral("source_file")).toString();
        if (!alreadyDeleted && !linkedFile.isEmpty() && QFileInfo(QDir(root).absoluteFilePath(linkedFile)).canonicalFilePath() != sourcePath) {
            return Thoughts::syncCommandError(QStringLiteral("source_conflict"),
                QStringLiteral("Thoughts links this comment to %1, not to this source. Sync, then try again.").arg(linkedFile));
        }
    }

    if (alreadyDeleted) {
        // The same verified edit as "Comment missing" > Remove marker; the next Sync publishes it.
        QVariantMap outcome;
        KParts::ReadWritePart *part = fedoraGatewayPart();
        const QVariantList markers{QVariantMap{{QStringLiteral("anchor_id"), anchorId}, {QStringLiteral("source_file"), relative}}};
        const bool invoked = part
            && QMetaObject::invokeMethod(part, "resolveMissingCommentMarkers", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, outcome),
                                         Q_ARG(QString, QStringLiteral("remove")), Q_ARG(QVariantList, markers));
        if (!invoked || !outcome.value(QStringLiteral("success")).toBool()) {
            const QString detail = outcome.value(QStringLiteral("results")).toList().value(0).toMap().value(QStringLiteral("message")).toString();
            return Thoughts::syncCommandError(QStringLiteral("delete_failed"),
                QStringLiteral("This comment is already deleted in Thoughts, but its marker could not be removed from the source%1")
                    .arg(detail.isEmpty() ? QStringLiteral(".") : QStringLiteral(": ") + detail));
        }
        QFile source(sourcePath);
        const QByteArray bytes = source.open(QIODevice::ReadOnly) ? source.readAll() : QByteArray();
        const bool removed = source.isOpen() && commentMarkerCount(QString::fromUtf8(bytes), anchorId, markdown) == 0;
        return QJsonObject {{QStringLiteral("schema_version"), 1}, {QStringLiteral("accepted"), true}, {QStringLiteral("terminal"), true},
                            {QStringLiteral("state"), QStringLiteral("deleted")}, {QStringLiteral("comment_id"), commentId},
                            {QStringLiteral("already_deleted"), true},
                            {QStringLiteral("contents"), comment.value(QStringLiteral("contents")).toString()},
                            {QStringLiteral("deletion_reason"), comment.value(QStringLiteral("deletion_reason")).toString()},
                            {QStringLiteral("message"), QStringLiteral("This comment was already deleted in Thoughts; its leftover marker was removed from the source.")},
                            {QStringLiteral("source"), relative}, {QStringLiteral("source_sha256"), sourceSha256(bytes)},
                            {QStringLiteral("marker_removal"), removed ? QStringLiteral("complete") : QStringLiteral("pending")}};
    }

    QList<Thoughts::TrackedCommentTarget> deleted;
    const QVariantMap outcome = applyTrackedCommentDeletion({{QString(), commentId, 0}}, &deleted);
    if (deleted.isEmpty()) {
        const QString detail = outcome.value(QStringLiteral("error")).toString();
        return Thoughts::syncCommandError(QStringLiteral("delete_failed"), detail.isEmpty() ? QStringLiteral("Thoughts could not delete the comment.") : detail);
    }
    QJsonObject result {{QStringLiteral("schema_version"), 1}, {QStringLiteral("accepted"), true}, {QStringLiteral("terminal"), true},
                        {QStringLiteral("state"), QStringLiteral("deleted")}, {QStringLiteral("comment_id"), commentId},
                        {QStringLiteral("contents"), comment.value(QStringLiteral("contents")).toString()},
                        {QStringLiteral("deletion_reason"), effectiveFedoraProjections().value(prefix + commentId).toObject().value(QStringLiteral("deletion_reason")).toString()},
                        {QStringLiteral("message"), QStringLiteral("Comment deleted. The next Sync deletes it on your other devices.")}};
    if (!outcome.value(QStringLiteral("success")).toBool() && !outcome.value(QStringLiteral("error")).toString().isEmpty()) {
        result.insert(QStringLiteral("warning"), outcome.value(QStringLiteral("error")).toString());
    }
    if (checkSource) {
        QFile source(sourcePath);
        const QByteArray bytes = source.open(QIODevice::ReadOnly) ? source.readAll() : QByteArray();
        // A marker left behind is removed again when Thoughts next reads its sources.
        const bool removed = source.isOpen() && commentMarkerCount(QString::fromUtf8(bytes), anchorId, markdown) == 0;
        result.insert(QStringLiteral("source"), relative);
        result.insert(QStringLiteral("source_sha256"), sourceSha256(bytes));
        result.insert(QStringLiteral("marker_removal"), removed ? QStringLiteral("complete") : QStringLiteral("pending"));
    }
    return result;
}

QJsonObject Shell::todoCommand(const QString &command, const QJsonObject &request)
{
    KParts::ReadWritePart *const part = fedoraGatewayPart();
    if (!part) {
        return Thoughts::syncCommandError(QStringLiteral("app_unavailable"), QStringLiteral("No Thoughts window is available for to-dos."));
    }
    const QJsonObject projections = effectiveFedoraProjections();
    const QDateTime now = QDateTime::currentDateTime();
    if (command == QLatin1String("list-todos")) {
        // Nothing at all is no synced state, which must not read as an empty To-dos list.
        if (projections.isEmpty()) {
            return Thoughts::syncCommandError(QStringLiteral("not_configured"), QStringLiteral("Thoughts has no synced data on this Mac yet. Sync, then try again."));
        }
        return Thoughts::TodoCommands::list(projections, request, now.date());
    }
    const QString zoneId = QString::fromUtf8(QTimeZone::systemTimeZoneId());
    const Thoughts::TodoCommands::Change change = command == QLatin1String("add-todo")
        ? Thoughts::TodoCommands::add(projections, request, Thoughts::CommentsOverview::standaloneNotePayload(QString(), now), zoneId, now.date())
        : Thoughts::TodoCommands::edit(projections, request, Thoughts::Platform::desktopOriginId(), now, zoneId, now.date());
    if (change.payload.isEmpty()) {
        return change.result;
    }
    bool queued = false;
    QMetaObject::invokeMethod(part, "enqueueLogicalCommentMutation", Qt::DirectConnection, Q_RETURN_ARG(bool, queued), Q_ARG(QVariantMap, change.payload.toVariantMap()));
    return queued ? change.result : Thoughts::syncCommandError(QStringLiteral("queue_failed"), QStringLiteral("The to-do could not be added to the local sync queue."));
}

QJsonObject Shell::startSyncCommand(const QString &target)
{
    if (!Thoughts::validSyncCommandTarget(target)) {
        return Thoughts::syncCommandError(QStringLiteral("invalid_request"), QStringLiteral("Target must be an exact PDF filename."), target);
    }

    reconcileOrphanedManualSync();
    QJsonObject equivalent;
    QJsonObject blocker;
    const QList<QJsonObject> operations = runningSyncOperationSummaries();
    for (const QJsonObject &operation : operations) {
        if (blocker.isEmpty()) {
            blocker = operation;
        }
        if (operation.value(QStringLiteral("kind")).toString() != QLatin1String("sync")) {
            continue;
        }
        const QString operationTarget = operation.value(QStringLiteral("target")).toString();
        const bool coversRequest = target.isEmpty() ? operationTarget.isEmpty() : (operationTarget.isEmpty() || operationTarget == target);
        if (coversRequest) {
            equivalent = operation;
            break;
        }
    }
    if (!equivalent.isEmpty()) {
        return Thoughts::syncCommandOperationResult(equivalent, true);
    }
    ThoughtsSyncCommandService *service = syncCommandService(true);
    if (!blocker.isEmpty() || (service && service->hasActiveManualSync()) || !m_activeBuildOperationId.isEmpty()) {
        const QString blockingId = !blocker.isEmpty() ? blocker.value(QStringLiteral("id")).toString() : ((service && service->hasActiveManualSync()) ? service->activeManualSyncOperationId() : m_activeBuildOperationId);
        return Thoughts::syncCommandError(QStringLiteral("busy"), QStringLiteral("Another Thoughts sync or build is already active."), target, blockingId);
    }

    KParts::ReadWritePart *part = fedoraGatewayPart();
    if (!part) {
        return Thoughts::syncCommandError(QStringLiteral("app_unavailable"), QStringLiteral("No Thoughts window is available for synchronization."), target);
    }

    TargetSyncMode targetMode = TargetSyncMode::SnapshotOnly;
    if (!target.isEmpty()) {
        const LocalLibraryEntry targetEntry = localLibraryEntry(ShellUtils::docsRoot(), target);
        targetMode = targetSyncMode(targetEntry.available, targetEntry.found, targetEntry.kind);
        if (targetMode == TargetSyncMode::Reconcile) {
            return Thoughts::syncCommandError(
                QStringLiteral("library_reconciliation_required"), QStringLiteral("The local Library cannot classify %1 yet. Run an all-documents sync to reconcile it with the server, then retry.").arg(target), target);
        }
    }

    const QString operationId = beginSyncOperation(QStringLiteral("sync"), target, QStringLiteral("queued"), QStringLiteral("command_api"));
    if (!setActiveManualSyncOperation(operationId, QStringLiteral("command_api"))) {
        if (!operationId.isEmpty()) {
            updateSyncOperation(operationId, Thoughts::SyncOperationState::Failed, QStringLiteral("failed"), i18n("Another Thoughts sync is already active"));
        }
        return Thoughts::syncCommandError(QStringLiteral("busy"), QStringLiteral("Another Thoughts sync or build is already active."), target, service ? service->activeManualSyncOperationId() : QString());
    }
    const bool invoked = target.isEmpty() ? QMetaObject::invokeMethod(part, "updateFromFedora", Qt::QueuedConnection, Q_ARG(QString, operationId)) : dispatchTargetSync(part, target, targetMode, operationId);
    if (!invoked) {
        updateSyncOperation(operationId, Thoughts::SyncOperationState::Failed, QStringLiteral("failed"), i18n("Sync could not be started"));
        clearActiveManualSyncOperation();
    }
    updatePdfSyncStatus();
    return syncCommandStatus(operationId);
}

QJsonObject Shell::syncCommandStatus(const QString &operationId) const
{
    if (operationId.isEmpty()) {
        return Thoughts::syncCommandError(QStringLiteral("invalid_request"), QStringLiteral("An operation ID is required."));
    }
    const QJsonObject operation = readSyncOperationSummary(operationId);
    if (!operation.isEmpty()) {
        return Thoughts::syncCommandOperationResult(operation);
    }
    return Thoughts::syncCommandError(QStringLiteral("unknown_operation"), QStringLiteral("Sync operation was not found."));
}

bool Shell::setActiveManualSyncOperation(const QString &operationId, const QString &initiationControl)
{
    ThoughtsSyncCommandService *service = syncCommandService(true);
    if (!service || !service->claimManualSync(this, operationId)) {
        return false;
    }
    s_automaticSync.syncStarted(automaticSyncInitiation(initiationControl));
    refreshAutomaticSyncNotice();
    return true;
}

// The claimed Sync could not be started.
void Shell::clearActiveManualSyncOperation()
{
    automaticSyncEnded(false, QVariantMap());
    if (ThoughtsSyncCommandService *service = syncCommandService(false)) {
        service->completeManualSync(this, m_activeManualSyncOperationId);
    } else {
        m_activeManualSyncOperationId.clear();
    }
}

bool Shell::hasProcessManualSyncOperation() const
{
    const ThoughtsSyncCommandService *service = syncCommandService(false);
    return service && service->hasActiveManualSync();
}

// The Fedora repository, and the Sync it runs, outlive any window, but the
// Sync's progress and its end reach the journal only through a window's tabs.
// When the window that started a Sync closed mid-Sync, the end reached nobody:
// the operation stayed claimed, the journal said "running" and every later
// Sync was ignored until a restart (2026-10-19). Take the repository's state
// for the claimed operation and record it here.
void Shell::reconcileOrphanedManualSync()
{
    ThoughtsSyncCommandService *service = syncCommandService(false);
    KParts::ReadWritePart *part = fedoraGatewayPart();
    if (!service || !service->hasActiveManualSync() || !part) {
        return;
    }
    QVariantMap manualSync;
    if (!QMetaObject::invokeMethod(part, "fedoraManualSyncState", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, manualSync))) {
        return;
    }
    // Between claiming an operation and starting it the repository still
    // reports the previous Sync; only its own operation's state counts.
    if (manualSync.value(QStringLiteral("operation_id")).toString() != service->activeManualSyncOperationId()) {
        return;
    }
    const int state = manualSync.value(QStringLiteral("state")).toInt();
    if (state < 1 || state > 3 || (state == 1 && m_fedoraManualSyncState == 1)) {
        return;
    }
    updateCommentsOverviewManualSyncState(state, manualSync.value(QStringLiteral("message")).toString(), manualSync.value(QStringLiteral("details")).toString());
}

void Shell::reconcileInterruptedSyncOperationsForTesting(qint64 startedBeforeMs)
{
    reconcileInterruptedSyncOperations(startedBeforeMs);
}

void Shell::triggerFedoraSync()
{
    triggerFedoraSyncWithInitiationControl(sender() ? QStringLiteral("pdf_reader_sync") : QStringLiteral("direct_sync_request"));
}

// The wait of the automatic Syncs began, changed or ended: every window's
// status area says so, and says so no longer once the wait is over.
void Shell::refreshAutomaticSyncNotice()
{
    static QPointer<QTimer> expiry;
    if (!expiry) {
        expiry = new QTimer(qApp);
        expiry->setSingleShot(true);
        QObject::connect(expiry, &QTimer::timeout, qApp, [] { refreshAutomaticSyncNotice(); });
    }
    const qint64 remaining = s_automaticSync.notBeforeMs() - QDateTime::currentMSecsSinceEpoch();
    if (remaining > 0) {
        expiry->start(int(remaining) + 500);
    } else {
        expiry->stop();
    }
    if (s_commentsOverview) {
        s_commentsOverview->setAutomaticSyncNotice(automaticSyncNotice());
    }
    const auto windows = KMainWindow::memberList();
    for (KMainWindow *window : windows) {
        if (auto *shell = qobject_cast<Shell *>(window)) {
            shell->schedulePdfSyncStatusUpdate();
        }
    }
}

void Shell::resetAutomaticSyncForTesting()
{
    s_automaticSync = Thoughts::AutomaticSyncPolicy();
    refreshAutomaticSyncNotice();
}

namespace
{
// The Sync that sends the user's proposal decisions (Shell::proposalDecided).
QPointer<QTimer> s_proposalDecisionSyncTimer;
bool s_proposalDecisionSyncWanted = false;
// ProposalDecisionSyncDelayMs, shorter in tests.
int s_proposalDecisionSyncDelayMs = ProposalDecisionSyncDelayMs;
}

// The user accepted or rejected a proposal on this computer, or undid that.
// A Sync sends it by itself ProposalDecisionSyncDelayMs after the last such
// decision, so a run of decisions goes as one Sync.
void Shell::proposalDecided()
{
    // A change the user made on this computer: automatic Syncs wait no longer.
    s_automaticSync.localChange();
    refreshAutomaticSyncNotice();
    if (!s_proposalDecisionSyncTimer) {
        s_proposalDecisionSyncTimer = new QTimer(qApp);
        s_proposalDecisionSyncTimer->setSingleShot(true);
        QObject::connect(s_proposalDecisionSyncTimer, &QTimer::timeout, qApp, [] { startProposalDecisionSync(); });
    }
    s_proposalDecisionSyncWanted = true;
    s_proposalDecisionSyncTimer->start(s_proposalDecisionSyncDelayMs);
}

void Shell::startProposalDecisionSync()
{
    if (!s_proposalDecisionSyncWanted) {
        return;
    }
    // Without a server there is nowhere to send it. And while the last failure
    // needs the user (a merge, a credential, documents that need attention) the
    // decision stays saved on this computer and goes with the user's next Sync.
    if (Thoughts::ServerAddress::normalized(Thoughts::ServerAddress::configured()).isEmpty() || s_automaticSync.lastFailureNeedsUser()) {
        s_proposalDecisionSyncWanted = false;
        return;
    }
    Shell *first = nullptr;
    bool busy = false;
    const auto windows = KMainWindow::memberList();
    for (KMainWindow *window : windows) {
        if (auto *shell = qobject_cast<Shell *>(window)) {
            first = first ? first : shell;
            busy = busy || shell->m_fedoraManualSyncState == 1 || shell->hasProcessManualSyncOperation() || !shell->m_activeBuildOperationId.isEmpty();
        }
    }
    if (!first || !first->fedoraGatewayPart()) {
        s_proposalDecisionSyncWanted = false;
        return;
    }
    // Never while a Sync or a build runs: one follows when it has ended. And an
    // automatic Sync that failed since the decision makes this one wait too.
    const qint64 wait = s_automaticSync.notBeforeMs() - QDateTime::currentMSecsSinceEpoch();
    if (busy || syncJournal()->compactionRunning() || wait > 0) {
        s_proposalDecisionSyncTimer->start(wait > 0 && !busy ? int(wait) + 500 : s_proposalDecisionSyncDelayMs);
        return;
    }
    s_proposalDecisionSyncWanted = false;
    first->triggerFedoraSyncWithInitiationControl(QStringLiteral("proposal_decision"));
}

void Shell::resetProposalDecisionSyncForTesting(int delayMs)
{
    s_proposalDecisionSyncDelayMs = delayMs > 0 ? delayMs : ProposalDecisionSyncDelayMs;
    s_proposalDecisionSyncWanted = false;
    if (s_proposalDecisionSyncTimer) {
        s_proposalDecisionSyncTimer->stop();
    }
}

// A Sync ended, or could not be started at all.
void Shell::automaticSyncEnded(bool success, const QVariantMap &manualSync)
{
    s_automaticSync.syncFinished(success, success ? Thoughts::AutomaticSyncPolicy::FailureKind::Unknown : automaticSyncFailureKind(manualSync), QDateTime::currentMSecsSinceEpoch());
    refreshAutomaticSyncNotice();
    if (s_proposalDecisionSyncWanted && s_proposalDecisionSyncTimer) {
        // The decisions made while this Sync ran go with one of their own.
        s_proposalDecisionSyncTimer->start(s_proposalDecisionSyncDelayMs);
    }
    // A Sync reads and verifies most of what the caches keep: record their size after each.
    recordCacheSizes();
    if (!success) {
        // What the server holds right after a failed Sync is that Sync's own
        // state: ask for it now, so it is not taken for news later.
        QTimer::singleShot(StateAfterFailedSyncDelayMs, this, &Shell::checkRemoteWork);
    }
}

// A Mac's menus traditionally carry no icons. Since macOS 26 Qt hands every
// action's icon to the native menu all the same, as one image with every size
// the icon theme offers (seven for most icons): for the menus of this app that
// was well over a hundred megabytes that nobody needed. Toolbars and buttons do
// not go through this and keep their icons.
void Shell::applyMenuIconPolicy()
{
#ifdef Q_OS_MACOS
    QCoreApplication::setAttribute(Qt::AA_DontShowIconsInMenus, true);
#endif
}

// What the caches hold now. A later measurement of the app's memory can be set
// against these lines of the developer log.
void Shell::recordCacheSizes()
{
    const Thoughts::VerifiedArtifactCacheStatistics artifacts = Thoughts::verifiedArtifactCacheStatistics();
    Thoughts::DeveloperLog::record(QStringLiteral("memory.caches"),
                                   {{QStringLiteral("artifact_bytes_entries"), artifacts.bytesEntries},
                                    {QStringLiteral("artifact_bytes_kept"), artifacts.bytesKept},
                                    {QStringLiteral("artifact_bytes_limit"), Thoughts::VerifiedArtifactBytesLimit},
                                    {QStringLiteral("source_artifacts_parsed"), artifacts.jsonEntries},
                                    {QStringLiteral("source_artifacts_estimated_bytes"), artifacts.jsonBytesKept},
                                    {QStringLiteral("source_artifacts_limit"), Thoughts::SourceArtifactJsonBytesLimit},
                                    {QStringLiteral("verified_identities"), artifacts.verifiedIdentities},
                                    {QStringLiteral("artifact_bytes_hits"), artifacts.bytesHits},
                                    {QStringLiteral("artifact_bytes_misses"), artifacts.bytesMisses},
                                    {QStringLiteral("source_artifact_hits"), artifacts.jsonHits},
                                    {QStringLiteral("source_artifact_misses"), artifacts.jsonMisses},
                                    {QStringLiteral("pixmap_cache_limit_kib"), QPixmapCache::cacheLimit()},
                                    {QStringLiteral("menu_icons"), !QCoreApplication::testAttribute(Qt::AA_DontShowIconsInMenus)}});
}

// The Sync journal is rewritten once so that removing old runs shrinks its file
// (SyncJournal::compact). The rewrite takes seconds and the journal answers
// nothing else meanwhile, so it waits until no Sync or build runs and Thoughts
// has not been the active application for SyncJournalCompactIdleMs.
void Shell::startSyncJournalUpkeep()
{
    static QPointer<QTimer> timer;
    if (timer) {
        return;
    }
    static QElapsedTimer inactive;
    if (QGuiApplication::applicationState() != Qt::ApplicationActive) {
        inactive.start();
    }
    QObject::connect(qApp, &QGuiApplication::applicationStateChanged, qApp, [](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive) {
            inactive.invalidate();
        } else if (!inactive.isValid()) {
            inactive.start();
        }
    });
    timer = new QTimer(qApp);
    timer->setInterval(SyncJournalUpkeepIntervalMs);
    QObject::connect(timer, &QTimer::timeout, qApp, [] {
        Thoughts::SyncJournal *journal = syncJournal();
        if (!journal->compactionPending() || journal->compactionRunning() || !inactive.isValid() || inactive.elapsed() < SyncJournalCompactIdleMs) {
            return;
        }
        const auto windows = KMainWindow::memberList();
        for (KMainWindow *window : windows) {
            const auto *shell = qobject_cast<Shell *>(window);
            if (shell && (shell->m_fedoraManualSyncState == 1 || shell->hasProcessManualSyncOperation() || !shell->m_activeBuildOperationId.isEmpty())) {
                return;
            }
        }
        journal->compact();
    });
    timer->start();
    // The status area answered from memory during the rewrite: ask again.
    QObject::connect(syncJournal(), &Thoughts::SyncJournal::compactionFinished, qApp, [] {
        const auto windows = KMainWindow::memberList();
        for (KMainWindow *window : windows) {
            if (auto *shell = qobject_cast<Shell *>(window)) {
                shell->schedulePdfSyncStatusUpdate();
            }
        }
    });
}

// The pause between two checks is the watch's: a plain 20 s, or, with a server
// that has sync_state_wait_v1, one request the server holds until its state
// changes, so that a change runs the check at once (syncstatewatch.h). Whether
// a check starts a Sync stays the policy's decision (remoteWorkChecked).
void Shell::startRemoteWorkWatch()
{
    if (s_remoteWorkWatch) {
        return;
    }
    s_remoteWorkWatch = new Thoughts::SyncStateWatch(qApp);
    QObject::connect(s_remoteWorkWatch, &Thoughts::SyncStateWatch::checkDue, qApp, [] {
        const auto windows = KMainWindow::memberList();
        for (KMainWindow *window : windows) {
            if (auto *shell = qobject_cast<Shell *>(window)) {
                shell->checkRemoteWork();
                return;
            }
        }
    });
    // A held request ends with the app; nothing is left waiting for the server.
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, s_remoteWorkWatch, &Thoughts::SyncStateWatch::stop);
    s_remoteWorkWatch->start();
}

Thoughts::SyncStateWatch *Shell::remoteWorkWatchForTesting()
{
    return s_remoteWorkWatch;
}

void Shell::checkRemoteWork()
{
    // No automatic Sync while the journal is rewritten: its start would wait for the rewrite.
    if (m_fedoraManualSyncState == 1 || hasProcessManualSyncOperation() || !m_activeBuildOperationId.isEmpty() || syncJournal()->compactionRunning()) {
        return;
    }
    KParts::ReadWritePart *part = fedoraGatewayPart();
    if (!part) {
        return;
    }
    connect(part, SIGNAL(fedoraRemoteWorkChecked(bool,QString)), this, SLOT(remoteWorkChecked(bool,QString)), Qt::UniqueConnection); // clazy:exclude=old-style-connect
    s_automaticSync.stateRequested(QDateTime::currentMSecsSinceEpoch());
    QMetaObject::invokeMethod(part, "checkFedoraRemoteWork", Qt::QueuedConnection);
}

void Shell::remoteWorkChecked(bool pending, const QString &stateKey)
{
    // Whether this state starts a Sync is the policy's decision: not while
    // automatic Syncs wait after failures, and not for the state a failed Sync
    // left.
    if (s_automaticSync.stateAnswered(pending, stateKey, QDateTime::currentMSecsSinceEpoch()) != Thoughts::AutomaticSyncPolicy::Decision::Start) {
        return;
    }
    if (m_fedoraManualSyncState == 1 || hasProcessManualSyncOperation() || !m_activeBuildOperationId.isEmpty()) {
        return;
    }
    s_automaticSync.automaticSyncStartedFor(stateKey);
    triggerFedoraSyncWithInitiationControl(QStringLiteral("remote_change"));
}

void Shell::remoteWorkAnnounced(bool pending, const QString &stateKey)
{
    // The policy decides here too, by its rule for an ended build: its Sync
    // starts at once, also while automatic Syncs wait after failures, but not
    // for a state a Sync already ran against. The parts of all tabs and windows
    // relay the one announcement; the first one starts the Sync and the others
    // find it started.
    if (s_automaticSync.stateAnnounced(pending, stateKey) != Thoughts::AutomaticSyncPolicy::Decision::Start) {
        return;
    }
    if (m_fedoraManualSyncState == 1 || hasProcessManualSyncOperation() || !m_activeBuildOperationId.isEmpty()) {
        // What runs now fetches the PDF, or the next periodic check finds it.
        return;
    }
    if (syncJournal()->compactionRunning()) {
        // The start would wait for the rewrite of the journal.
        QTimer::singleShot(AnnouncedWorkRetryMs, this, [this, pending, stateKey] { remoteWorkAnnounced(pending, stateKey); });
        return;
    }
    s_automaticSync.automaticSyncStartedFor(stateKey);
    triggerFedoraSyncWithInitiationControl(QStringLiteral("remote_change"));
}

void Shell::triggerFedoraSyncWithInitiationControl(const QString &initiationControl)
{
    reconcileOrphanedManualSync();
    if (m_fedoraManualSyncState == 1 || hasProcessManualSyncOperation() || !m_activeBuildOperationId.isEmpty()) {
        return;
    }
    KParts::ReadWritePart *part = fedoraGatewayPart();
    if (!part) {
        if (s_commentsOverview) {
            s_commentsOverview->syncFinished(false, i18n("No Thoughts window is available for synchronization."));
        } else {
            KMessageBox::error(this, i18n("No Thoughts window is available for synchronization."), i18n("Sync"));
        }
        return;
    }
    const QString operationId = beginSyncOperation(QStringLiteral("sync"), QString(), QStringLiteral("queued"), initiationControl);
    if (!setActiveManualSyncOperation(operationId, initiationControl)) {
        if (!operationId.isEmpty()) {
            updateSyncOperation(operationId, Thoughts::SyncOperationState::Failed, QStringLiteral("failed"), i18n("Another Thoughts sync is already active"));
        }
        if (s_commentsOverview) {
            s_commentsOverview->syncFinished(false, i18n("Another synchronization is already active."));
        }
        return;
    }
    if (s_commentsOverview) {
        s_commentsOverview->syncStarted();
    }
    if (!QMetaObject::invokeMethod(part, "updateFromFedora", Qt::QueuedConnection, Q_ARG(QString, m_activeManualSyncOperationId))) {
        const QString failedOperationId = m_activeManualSyncOperationId;
        clearActiveManualSyncOperation();
        updateSyncOperation(failedOperationId, Thoughts::SyncOperationState::Failed, QStringLiteral("failed"), i18n("Sync could not be started"));
        if (s_commentsOverview) {
            s_commentsOverview->syncFinished(false, i18n("Synchronization is unavailable."));
        } else {
            KMessageBox::error(this, i18n("Synchronization is unavailable."), i18n("Sync"));
        }
    }
    updateCommentsOverviewDocumentSyncPresentations();
    updatePdfSyncStatus();
}

void Shell::updateCommentsOverviewManualSyncState(int state, const QString &message, const QString &details)
{
    const bool wasSyncing = m_fedoraManualSyncState == 1;
    m_fedoraManualSyncState = state;
    if (m_reviewOverview) { m_reviewOverview->setSyncInProgress(state == 1); }
    if (wasSyncing && state != 1) {
        const QJsonObject projections = effectiveFedoraProjections();
        for (const auto &tab : std::as_const(m_tabs)) { refreshSidebarReviewCards(tab.part, projections); }
        // A merge waiting for this Sync: once only, so a Sync that did not send the source shows why.
        if (m_mergeAfterSync) {
            const PendingCardMerge pending = *m_mergeAfterSync;
            m_mergeAfterSync.reset();
            QTimer::singleShot(0, this, [this, pending] {
                if (pending.part) mergeReviewCardsIn(pending.part, pending.cardIds, true);
            });
        }
    }
    QVariantMap manualSync;
    if (KParts::ReadWritePart *part = fedoraGatewayPart()) {
        QMetaObject::invokeMethod(part, "fedoraManualSyncState", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, manualSync));
    }
    m_fedoraManualSyncTarget = manualSync.value(QStringLiteral("target")).toString();
    m_fedoraManualSyncMessage = message;
    m_fedoraManualSyncDetails = details;
    QString operationId;
    ThoughtsSyncCommandService *service = syncCommandService(true);
    const bool journalState = service && service->prepareManualSyncState(this, state, m_fedoraManualSyncTarget, message, details, &operationId);
    if (journalState && state == 1) {
        updateSyncOperation(operationId, Thoughts::SyncOperationState::Running, QStringLiteral("downloading"), Thoughts::SyncStatusLabels::syncing());
    } else if (journalState && state == 2) {
        int unresolvedLinks = 0;
        const QJsonObject projections = effectiveFedoraProjections();
        const QString target = QFileInfo(m_fedoraManualSyncTarget).completeBaseName();
        for (auto it = projections.begin(); it != projections.end(); ++it) {
            if (!it.key().startsWith(QLatin1String("logical_comment\ncomments\n"))) continue;
            const QJsonObject comment = it.value().toObject();
            if (comment.value(QStringLiteral("deleted")).toBool() || !comment.value(QStringLiteral("parent_id")).toString().isEmpty()) continue;
            const QString collection = comment.value(QStringLiteral("origin")).toObject().value(QStringLiteral("collection")).toString();
            if (!target.isEmpty() && target != QLatin1String("all") && target != QLatin1String("All documents") && target != collection) continue;
            const QString status = comment.value(QStringLiteral("linking")).toObject().value(QStringLiteral("status")).toString();
            // Anchor-removed comments are counted as Issues, not as comments that need linking.
            const QJsonObject scope = comment.value(QStringLiteral("scope")).toObject();
            if (Thoughts::isAnchorRemovedScope(scope)) continue;
            if (status == QLatin1String("needs_attention") || status == QLatin1String("failed")
                || scope.value(QStringLiteral("state")).toString() == QLatin1String("needs_attention")) ++unresolvedLinks;
        }
        const QString completion = unresolvedLinks > 0
            ? i18np("Synced; 1 comment needs linking", "Synced; %1 comments need linking", unresolvedLinks)
            : i18n("Completed");
        // A Sync whose changes arrived succeeds also when the server could not build a
        // PDF: the repository says so as a warning, which the run keeps for whoever asks
        // about this Sync afterwards (thoughtsctl and the proposal commands).
        updateSyncOperation(operationId, Thoughts::SyncOperationState::Succeeded, QStringLiteral("succeeded"), completion, QString(), QString(),
                            manualSync.value(QStringLiteral("warning_code")).toString(), manualSync.value(QStringLiteral("warning")).toString());
        service->completeManualSync(this, operationId);
        automaticSyncEnded(true, manualSync);
    } else if (journalState && state == 3) {
        const QString failure = Thoughts::syncOperationFailureJournalMessage(message.trimmed().isEmpty() ? Thoughts::SyncStatusLabels::syncFailed() : message, details);
        updateSyncOperation(operationId, Thoughts::SyncOperationState::Failed, QStringLiteral("failed"), failure, QString(),
                            manualSync.value(QStringLiteral("error_code")).toString());
        service->completeManualSync(this, operationId);
        automaticSyncEnded(false, manualSync);
    }
    updatePdfSyncStatus();
    updateCommentsOverviewDocumentSyncPresentations();
    const bool overviewNeedsTerminalState = s_commentsOverview && s_commentsOverview->syncOperationRunning() && (state == 2 || state == 3);
    if (s_commentsOverview && (journalState || overviewNeedsTerminalState)) {
        switch (state) {
        case 1:
            s_commentsOverview->syncStarted();
            break;
        case 2:
            s_commentsOverview->syncFinished(true, QString());
            break;
        case 3:
            s_commentsOverview->syncFinished(false, message, details, manualSync.value(QStringLiteral("error_code")).toString(), operationId);
            break;
        default:
            break;
        }
    }
    updatePdfLibraryAction();
}

void Shell::updateSyncOperationProgress(const QString &traceId, const QString &phase, qint64 transferredBytes, qint64 totalBytes, const QString &message)
{
    if (repeatedSyncSignal(traceId, QJsonObject{{QStringLiteral("progress_phase"), phase}, {QStringLiteral("transferred"), transferredBytes},
                                                {QStringLiteral("total"), totalBytes}, {QStringLiteral("message"), message}})) return;
    syncJournal()->appendProgress(traceId, phase, transferredBytes, totalBytes, message);
    Q_EMIT syncOperationJournalChanged(traceId);
    schedulePdfSyncStatusUpdate();
}

void Shell::updateSyncOperationTransferProgress(const QString &traceId, const QJsonObject &absoluteSnapshot)
{
    QJsonObject normalized;
    if (traceId.isEmpty() || !Thoughts::normalizedSyncTransferSnapshot(absoluteSnapshot, &normalized)) {
        return;
    }
    m_pendingSyncTransferSnapshots.insert(traceId, normalized);
    if (normalized.value(QStringLiteral("flush")).toBool()) {
        flushPendingSyncTransferProgress(traceId);
        return;
    }
    if (!m_syncTransferProgressTimer) {
        m_syncTransferProgressTimer = new QTimer(this);
        m_syncTransferProgressTimer->setSingleShot(true);
        m_syncTransferProgressTimer->setInterval(200);
        connect(m_syncTransferProgressTimer, &QTimer::timeout, this, [this] { flushPendingSyncTransferProgress(); });
    }
    if (!m_syncTransferProgressTimer->isActive()) {
        m_syncTransferProgressTimer->start();
    }
}

void Shell::flushPendingSyncTransferProgress(const QString &traceId)
{
    QHash<QString, QJsonObject> snapshots;
    if (traceId.isEmpty()) {
        snapshots.swap(m_pendingSyncTransferSnapshots);
    } else {
        const auto found = m_pendingSyncTransferSnapshots.find(traceId);
        if (found != m_pendingSyncTransferSnapshots.end()) {
            snapshots.insert(found.key(), found.value());
            m_pendingSyncTransferSnapshots.erase(found);
        }
    }
    if (m_pendingSyncTransferSnapshots.isEmpty() && m_syncTransferProgressTimer) {
        m_syncTransferProgressTimer->stop();
    }
    for (auto iterator = snapshots.cbegin(); iterator != snapshots.cend(); ++iterator) {
        syncJournal()->updateTransferProgress(iterator.key(), iterator.value());
        Q_EMIT syncOperationJournalChanged(iterator.key());
    }
    if (!snapshots.isEmpty()) {
        updatePdfSyncStatus();
    }
}

// Every open tab of every window relays the repository's Sync signals. Each relayed copy of the
// same event was stored in its own SQLite transaction on the GUI thread; keep only the first.
static bool repeatedSyncSignal(const QString &traceId, const QJsonObject &event)
{
    static QList<std::pair<QString, QJsonObject>> recent;
    for (const auto &[id, seen] : std::as_const(recent)) {
        if (id == traceId && seen == event) return true;
    }
    recent.append({traceId, event});
    if (recent.size() > 64) recent.removeFirst();
    return false;
}

void Shell::updateSyncOperationJournalEvent(const QString &traceId, const QJsonObject &event)
{
    if (repeatedSyncSignal(traceId, event)) return;
    syncJournal()->appendEvent(traceId, event);
    Q_EMIT syncOperationJournalChanged(traceId);
    schedulePdfSyncStatusUpdate();
}

void Shell::updateCommentsOverviewRenderState(const QJsonObject &state)
{
    if (s_commentsOverview) {
        s_commentsOverview->setPdfSyncState(state);
    }
}

void Shell::updateCommentsOverviewAgentProposalResolution(const QString &operationId, const QStringList &proposalIds, const QString &action, const QString &status, const QString &message)
{
    // Saved on this computer ("applied"), also when the server could not be
    // told yet ("waiting"). A decision that waited reports "applied" once a
    // Sync delivered it: that is no new decision.
    if (status == QLatin1String("applied") || status == QLatin1String("waiting")) {
        static QStringList counted;
        if (operationId.isEmpty() || !counted.contains(operationId)) {
            if (!operationId.isEmpty()) {
                counted.append(operationId);
                if (counted.size() > 256) {
                    counted.removeFirst();
                }
            }
            proposalDecided();
        }
    }
    if (s_commentsOverview) {
        s_commentsOverview->finishAgentProposalResolution(proposalIds, action, status, message);
    }
}

Shell *Shell::shellForCommentsOverview(const QString &pdfPath)
{
    QList<Shell *> shells;
    for (KMainWindow *window : KMainWindow::memberList()) {
        if (auto *shell = qobject_cast<Shell *>(window)) {
            shells.append(shell);
        }
    }

    if (!pdfPath.isEmpty()) {
        const QUrl url = QUrl::fromLocalFile(QFileInfo(pdfPath).absoluteFilePath());
        for (Shell *shell : std::as_const(shells)) {
            if (shell->findTabIndex(url) >= 0) {
                s_commentsOverviewShell = shell;
                return shell;
            }
        }
    }

    if (s_commentsOverviewShell && s_commentsOverviewShell->isVisible()) {
        return s_commentsOverviewShell;
    }
    for (Shell *shell : std::as_const(shells)) {
        if (shell->isVisible() && shell->isActiveWindow()) {
            s_commentsOverviewShell = shell;
            return shell;
        }
    }
    for (Shell *shell : std::as_const(shells)) {
        if (shell->isVisible()) {
            s_commentsOverviewShell = shell;
            return shell;
        }
    }
    if (s_commentsOverviewShell) {
        return s_commentsOverviewShell;
    }

    auto *shell = new Shell;
    if (!shell->isValid()) {
        delete shell;
        return nullptr;
    }
    s_commentsOverviewShell = shell;
    return shell;
}

void Shell::showReviewCardInLibrary(const QString &cardId)
{
    Shell *shell = shellForCommentsOverview();
    Thoughts::CommentsOverview *overview = commentsOverview();
    s_commentsOverviewShell = shell;
    shell->updateCommentsOverviewDocumentSyncPresentations();
    overview->showReviewCardInfo(cardId);
    overview->setAttribute(Qt::WA_ShowWithoutActivating, false);
    overview->show();
    overview->raise();
    overview->activateWindow();
}

void Shell::openPageLink(const QUrl &url)
{
    Shell *shell = shellForCommentsOverview();
    QWidget *parent = QApplication::activeWindow() ? QApplication::activeWindow() : static_cast<QWidget *>(shell);
    const std::optional<Thoughts::PageLink> link = Thoughts::parsePageLink(url);
    if (!link) {
        KMessageBox::error(parent, Thoughts::pageLinkInvalidMessage(), i18n("Open Page Link"));
        return;
    }
    const QString docsRoot = ShellUtils::docsRoot();
    const QString path = Thoughts::pageLinkDocumentPath(docsRoot, link->documentId);
    const QString currentVersion = path.isEmpty() ? QString() : Thoughts::pageLinkPdfSha256(docsRoot, path);
    Shell *destination = path.isEmpty() ? nullptr : shellForCommentsOverview(path);
    if (currentVersion.isEmpty() || !destination) {
        KMessageBox::error(parent, Thoughts::pageLinkDocumentMissingMessage(), i18n("Open Page Link"));
        return;
    }
    QString error;
    KParts::ReadWritePart *part = destination->trackedCommentPart(path, true, &error);
    uint pages = 0;
    if (part && (!QMetaObject::invokeMethod(part, "pages", Qt::DirectConnection, Q_RETURN_ARG(uint, pages)) || !pages)) {
        error = i18n("The PDF could not be opened: %1", QDir::toNativeSeparators(path));
        part = nullptr;
    }
    if (!part) {
        KMessageBox::error(parent, error, i18n("Open Page Link"));
        return;
    }
    const Thoughts::PageLinkOpening opening = Thoughts::pageLinkOpening(link->targetPage(), int(pages), currentVersion != link->version);
    QMetaObject::invokeMethod(part, "goToPage", Qt::DirectConnection, Q_ARG(uint, uint(opening.page)));
    destination->selectReaderTitle(part, link->documentId, QString(), QString());
    if (destination->isMinimized()) {
        destination->showNormal();
    } else {
        destination->show();
    }
    destination->raise();
    destination->activateWindow();
    if (!opening.message.isEmpty()) {
        // Like the other notes about a followed link: on the page, without stopping the reader.
        QMetaObject::invokeMethod(part, "noticeMessage", Qt::DirectConnection, Q_ARG(QString, opening.message), Q_ARG(int, -1));
    }
}

QString Shell::agentNoteDocumentPath(const QString &documentId, const QString &filename, QString *title)
{
    const QString docsRoot = ShellUtils::docsRoot();
    QString path = Thoughts::DocumentIdentityStore(docsRoot).readablePath(documentId);
    if (path.isEmpty() && !filename.isEmpty() && !filename.contains(QLatin1Char('/'))) {
        const QString published = QDir(docsRoot).filePath(QStringLiteral("current_pdfs/") + filename);
        if (QFileInfo(published).isFile()) path = published;
    }
    if (!path.isEmpty() && title) {
        Shell *shell = shellForCommentsOverview(path);
        *title = shell && shell->m_readerDisplayTitles
            ? shell->m_readerDisplayTitles->title(QUrl::fromLocalFile(path), documentId, QString(), QFileInfo(path).completeBaseName())
            : QFileInfo(path).completeBaseName();
    }
    return path;
}

void Shell::openAgentNoteLink(const QString &href)
{
    QWidget *parent = QApplication::activeWindow();
    // A Library PDF page, as the note tool writes it and the phone opens it (0-based page).
    static const QRegularExpression pdfLink(QStringLiteral(R"(^thoughts-pdf-link:([A-Za-z0-9][A-Za-z0-9_.-]*\.pdf)\?page=([0-9]{1,6})$)"));
    if (const QRegularExpressionMatch match = pdfLink.match(href); match.hasMatch()) {
        const QString filename = match.captured(1);
        const QString path = agentNoteDocumentPath(QString(), filename, nullptr);
        Shell *shell = path.isEmpty() ? nullptr : shellForCommentsOverview(path);
        if (!shell) {
            KMessageBox::information(parent, i18n("“%1” is not in this Mac's Library yet. Sync, then click the link again.", filename), i18n("Open Link"));
            return;
        }
        QString error;
        KParts::ReadWritePart *part = shell->trackedCommentPart(path, true, &error);
        uint pages = 0;
        if (!part || !QMetaObject::invokeMethod(part, "pages", Qt::DirectConnection, Q_RETURN_ARG(uint, pages)) || !pages) {
            KMessageBox::error(parent, error.isEmpty() ? i18n("The PDF could not be opened: %1", QDir::toNativeSeparators(path)) : error, i18n("Open Link"));
            return;
        }
        QMetaObject::invokeMethod(part, "goToPage", Qt::DirectConnection, Q_ARG(uint, uint(std::min<qint64>(match.captured(2).toLongLong(), pages - 1))));
        const QString documentId = Thoughts::DocumentIdentityStore(ShellUtils::docsRoot()).forFile(path).value(QStringLiteral("document_id")).toString();
        shell->selectReaderTitle(part, documentId, QString(), QString());
        if (shell->isMinimized()) shell->showNormal();
        else shell->show();
        shell->raise();
        shell->activateWindow();
        return;
    }
    const QUrl url(href);
    if ((url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("http")) && url.isValid()) {
        if (!QDesktopServices::openUrl(url)) KMessageBox::error(parent, i18n("The link could not be opened: %1", href), i18n("Open Link"));
        return;
    }
    KMessageBox::error(parent, i18n("Thoughts cannot open this link: %1", href), i18n("Open Link"));
}

void Shell::openAgentNoteDocument(const QString &documentId, const QString &path)
{
    Shell *shell = shellForCommentsOverview(path);
    if (!shell) return;
    QString error;
    KParts::ReadWritePart *part = shell->trackedCommentPart(path, true, &error);
    if (!part) {
        KMessageBox::error(QApplication::activeWindow() ? QApplication::activeWindow() : static_cast<QWidget *>(shell), error);
        return;
    }
    shell->selectReaderTitle(part, documentId, QString(), QString());
    shell->show();
    shell->raise();
    shell->activateWindow();
}

namespace
{
// The Library's documents by project, from the project catalogue this Mac keeps.
QList<Thoughts::AgentNotes::LibraryDocument> agentNotesLibrary()
{
    QFile file(QDir(ShellUtils::docsRoot()).filePath(QStringLiteral(".thoughts/library-projects.json")));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return Thoughts::AgentNotes::libraryDocuments(QJsonDocument::fromJson(file.readAll()).object());
}
}

Thoughts::AgentNotesView *Shell::createAgentNotesView(bool sidebar, KParts::ReadOnlyPart *readerPart)
{
    const auto resolve = [](const QString &documentId, const QString &filename) {
        Thoughts::AgentNotesView::DocumentLink link;
        link.path = agentNoteDocumentPath(documentId, filename, &link.title);
        return link;
    };
    const auto openCard = [](const QString &cardId) { Shell::showReviewCardInLibrary(cardId); };
    // The page on screen in a reader, to send to the agent (a local PDF only: Poppler draws it).
    const auto capturePage = [](KParts::ReadOnlyPart *part) -> std::optional<Thoughts::AgentNotesView::PageCapture> {
        if (!part || !part->url().isLocalFile()) return std::nullopt;
        const QString path = part->url().toLocalFile();
        if (!path.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive) || !QFileInfo(path).isFile()) return std::nullopt;
        uint current = 0;
        uint pages = 0;
        QMetaObject::invokeMethod(part, "currentPage", Qt::DirectConnection, Q_RETURN_ARG(uint, current));
        QMetaObject::invokeMethod(part, "pages", Qt::DirectConnection, Q_RETURN_ARG(uint, pages));
        if (current == 0 || pages == 0) return std::nullopt;
        Thoughts::AgentNotesView::PageCapture capture;
        capture.page = int(current);
        capture.pages = int(pages);
        const QString docsRoot = ShellUtils::docsRoot();
        capture.documentId = Thoughts::DocumentIdentityStore(docsRoot).forFile(path).value(QStringLiteral("document_id")).toString();
        if (QFileInfo(path).absolutePath() == QFileInfo(QDir(docsRoot).filePath(QStringLiteral("current_pdfs"))).absoluteFilePath()) {
            capture.documentFilename = QFileInfo(path).fileName();
        }
        Shell *shell = shellForCommentsOverview(path);
        capture.documentName = shell && shell->m_readerDisplayTitles
            ? shell->m_readerDisplayTitles->title(part->url(), capture.documentId, QString(), QFileInfo(path).completeBaseName())
            : QFileInfo(path).completeBaseName();
        const int index = capture.page - 1;
        capture.render = [path, index](int width) -> QImage {
            std::unique_ptr<Poppler::Document> document = Poppler::Document::load(path);
            if (!document || document->isLocked()) return {};
            document->setRenderHint(Poppler::Document::Antialiasing);
            document->setRenderHint(Poppler::Document::TextAntialiasing);
            std::unique_ptr<Poppler::Page> page = document->page(index);
            if (!page) return {};
            const QSizeF points = page->pageSizeF();
            const double dpi = points.width() > 0 ? width * 72.0 / points.width() : 144.0;
            return page->renderToImage(dpi, dpi);
        };
        return capture;
    };
    Thoughts::AgentNotes::Center *center = Thoughts::AgentNotes::Center::instance();
    if (!sidebar) {
        auto *window = new Thoughts::AgentNotesWindow(center, resolve, &Shell::openAgentNoteDocument, openCard);
        window->setLibraryProvider(agentNotesLibrary);
        window->setLinkOpener(&Shell::openAgentNoteLink);
        // The window sends the page of the reader in front (never a new reader window).
        window->setPageProvider([capturePage]() -> std::optional<Thoughts::AgentNotesView::PageCapture> {
            Shell *shell = nullptr;
            int best = -1;
            for (KMainWindow *member : KMainWindow::memberList()) {
                auto *candidate = qobject_cast<Shell *>(member);
                if (!candidate || !candidate->isVisible()) continue;
                const int score = candidate->isActiveWindow() ? 2 : candidate == s_commentsOverviewShell ? 1 : 0;
                if (score > best) {
                    best = score;
                    shell = candidate;
                }
            }
            if (!shell || !shell->m_tabWidget) return std::nullopt;
            const int tab = shell->m_tabWidget->currentIndex();
            return tab >= 0 && tab < shell->m_tabs.size() ? capturePage(shell->m_tabs.at(tab).part) : std::nullopt;
        });
        return window;
    }
    auto *view = new Thoughts::AgentNotesView(Thoughts::AgentNotesView::Layout::Sidebar, center, resolve, &Shell::openAgentNoteDocument, openCard);
    view->setLibraryProvider(agentNotesLibrary);
    view->setLinkOpener(&Shell::openAgentNoteLink);
    // Too big for the sidebar: the note in the Agent Notes window.
    view->setLargeOpener([](const QString &noteId) { Shell::showAgentNotes(noteId); });
    // A sidebar sends the page beside it.
    QPointer<KParts::ReadOnlyPart> guarded(readerPart);
    view->setPageProvider([guarded, capturePage] { return capturePage(guarded); });
    return view;
}

Thoughts::AgentNotesWindow *Shell::agentNotesWindow()
{
    if (s_agentNotesWindow) {
        return s_agentNotesWindow;
    }
    s_agentNotesWindow = static_cast<Thoughts::AgentNotesWindow *>(createAgentNotesView(false));
    s_agentNotesWindow->setAttribute(Qt::WA_DeleteOnClose, false);
    return s_agentNotesWindow;
}

void Shell::attachAgentNotesSidebar(KParts::ReadOnlyPart *part)
{
    if (!part) return;
    // The entry shows once the part has its panel: a tab moved from another window brings it
    // along (in the part's side container, which the Shell keeps in its sidebar dock).
    QAction *entry = part->actionCollection()->action(QStringLiteral("sidebar_display_agent_notes"));
    if (!entry || entry->isVisible()) return;
    Thoughts::AgentNotesView *panel = createAgentNotesView(true, part);
    bool attached = false;
    QMetaObject::invokeMethod(part, "setAgentNotesSidebarPanel", Qt::DirectConnection, Q_RETURN_ARG(bool, attached), Q_ARG(QWidget *, panel));
    if (!attached) delete panel;
}

void Shell::showAgentNotes(const QString &noteId)
{
    agentNotesWindow()->showNote(noteId);
}

void Shell::showAgentNotes(const QString &noteId, const Thoughts::AgentNotes::Filter &filter)
{
    Thoughts::AgentNotesWindow *window = agentNotesWindow();
    if (!(window->filter() == filter)) window->setFilter(filter);
    window->showNote(noteId);
}

QJsonObject Shell::openAgentNotesCommand(const QJsonObject &request)
{
    const auto text = [&request](const char *key) { return request.value(QLatin1String(key)).toString().trimmed(); };
    for (const char *key : {"document", "project", "note_id"}) {
        const QJsonValue value = request.value(QLatin1String(key));
        if (!value.isUndefined() && !value.isNull() && (!value.isString() || value.toString().size() > 1024)) {
            return Thoughts::syncCommandError(QStringLiteral("invalid_request"), QStringLiteral("%1 must be a string.").arg(QLatin1String(key)));
        }
    }
    const QString document = text("document");
    const QString project = text("project");
    const QString noteId = text("note_id");
    if (!document.isEmpty() && !project.isEmpty()) {
        return Thoughts::syncCommandError(QStringLiteral("invalid_request"), QStringLiteral("Give a document or a project, not both."));
    }
    Thoughts::AgentNotes::Filter filter;
    if (!project.isEmpty() || !document.isEmpty()) {
        const QList<Thoughts::AgentNotes::LibraryDocument> library = agentNotesLibrary();
        std::optional<Thoughts::AgentNotes::Filter> found;
        if (!project.isEmpty()) {
            found = Thoughts::AgentNotes::Filter::project(library, project);
            if (!found) {
                return Thoughts::syncCommandError(QStringLiteral("not_found"), QStringLiteral("No Library project has the id or name \"%1\".").arg(project));
            }
        } else {
            QString wanted = document;
            // A PDF path: its Library identity, else its filename.
            if (QDir::isAbsolutePath(document)) {
                const QJsonObject identity = Thoughts::DocumentIdentityStore(ShellUtils::docsRoot()).forFile(document);
                wanted = identity.value(QStringLiteral("document_id")).toString();
                if (wanted.isEmpty()) wanted = QFileInfo(document).fileName();
            }
            found = Thoughts::AgentNotes::Filter::document(library, wanted);
            // A document the catalogue does not list (yet): its notes by document_id.
            if (!found && Thoughts::DocumentIdentityStore::validId(wanted)) {
                Thoughts::AgentNotes::Filter raw;
                raw.kind = QStringLiteral("document");
                raw.key = wanted;
                raw.label = wanted.left(8);
                raw.documentIds = {wanted};
                found = raw;
            }
            if (!found) {
                return Thoughts::syncCommandError(QStringLiteral("not_found"),
                                                  QStringLiteral("No Library document has the id, filename or name \"%1\".").arg(document));
            }
        }
        filter = *found;
    }
    showAgentNotes(noteId, filter);
    Thoughts::AgentNotesWindow *window = agentNotesWindow();
    QJsonObject result{{QStringLiteral("schema_version"), 1}, {QStringLiteral("accepted"), true}, {QStringLiteral("terminal"), true},
                       {QStringLiteral("state"), QStringLiteral("opened")}, {QStringLiteral("notes_listed"), window->listedCount()},
                       {QStringLiteral("loaded"), Thoughts::AgentNotes::Center::instance()->hasListed()}};
    if (!filter.isEmpty()) {
        result.insert(QStringLiteral("filter"), QJsonObject{{QStringLiteral("kind"), filter.kind}, {QStringLiteral("id"), filter.key},
                                                            {QStringLiteral("label"), filter.label}, {QStringLiteral("document_ids"), QJsonArray::fromStringList(filter.documentIds)}});
    }
    if (!noteId.isEmpty()) result.insert(QStringLiteral("note_id"), noteId);
    return result;
}

void Shell::updateAgentNotesAction(int unread)
{
    QAction *action = actionCollection()->action(QStringLiteral("agent_notes"));
    if (!action) return;
    action->setIcon(QIcon(new AgentNotesToolbarIconEngine(unread > 0)));
    action->setToolTip(unread > 0 ? i18np("Agent notes: 1 unread", "Agent notes: %1 unread", unread)
                                  : i18n("Read the notes agents sent to your phone, with your replies"));
}

void Shell::showCommentsOverview(bool activate)
{
    Thoughts::CommentsOverview *overview = commentsOverview();
    s_commentsOverviewShell = this;
    // Load durable local tombstones before the asynchronous Library catalog
    // becomes visible, including after an application restart.
    updateCommentsOverviewDocumentSyncPresentations();
    overview->showSummary();
    overview->setAttribute(Qt::WA_ShowWithoutActivating, !activate);
    overview->show();
    if (activate) {
        overview->raise();
        overview->activateWindow();
    }
}

void Shell::openCommentCodex(const QString &pdfPath, const QString &annotationId)
{
    openCodexForComment(this, pdfPath, annotationId);
}

void Shell::openCommentChatGpt(const QString &url)
{
    QString error;
    const QString normalized = Okular::DocsCommentSynchronizer::normalizedChatGptUrl(url, &error);
    if (!error.isEmpty() || normalized.isEmpty()) {
        KMessageBox::error(this, i18n("Enter a valid HTTPS chatgpt.com link."), i18n("Open ChatGPT"));
        return;
    }
    if (!QDesktopServices::openUrl(QUrl(normalized))) {
        KMessageBox::error(this, i18n("The ChatGPT link could not be opened."), i18n("Open ChatGPT"));
    }
}

void Shell::openCommentSource(const QString &sourcePath, int line)
{
    QString error;
    if (!openSourceInVisualStudioCode(sourcePath, line, &error)) {
        KMessageBox::error(this, error, i18n("Open Source in VS Code"));
    }
}

void Shell::openSourceSelection(const QString &sourcePath, int line, const QString &selectedText)
{
    openSourceComparison(sourcePath, line, selectedText);
}

void Shell::openSourceComparison(const QString &sourcePath, int line, const QString &selectedText, const QString &originalPath, const QString &failedSha256)
{
    if (selectedText.trimmed().isEmpty() && originalPath.isEmpty()) {
        openCommentSource(sourcePath, line);
        return;
    }
    const QString executable = visualStudioCodeExecutable();
    const QFileInfo source(sourcePath);
    if (executable.isEmpty() || !source.isFile() || line < 1) {
        openCommentSource(sourcePath, line);
        return;
    }
    const QString cache = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath(QStringLiteral("source-selection"));
    QDir().mkpath(cache);
    // Requests can contain private document text; retain them only briefly.
    for (const QFileInfo &old : QDir(cache).entryInfoList({QStringLiteral("thoughts-source-selection-*.json")}, QDir::Files)) {
        if (old.lastModified().secsTo(QDateTime::currentDateTime()) > 86400) {
            QFile::remove(old.absoluteFilePath());
        }
    }
    const QString requestPath = QDir(cache).filePath(QStringLiteral("thoughts-source-selection-%1.json").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QSaveFile request(requestPath);
    const QByteArray payload = QJsonDocument(QJsonObject{
        {QStringLiteral("path"), source.canonicalFilePath()},
        {QStringLiteral("line"), line},
        {QStringLiteral("text"), selectedText},
        {QStringLiteral("original_path"), originalPath},
        {QStringLiteral("failed_sha256"), failedSha256},
        {QStringLiteral("current_source"), !failedSha256.isEmpty()},
    }).toJson(QJsonDocument::Compact);
    if (!request.open(QIODevice::WriteOnly) || !request.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || request.write(payload) != payload.size() || !request.commit()) {
        KMessageBox::error(this, i18n("Could not prepare the source selection."));
        return;
    }
    QUrl url(QStringLiteral("vscode://thoughts.source-selection/open"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("request"), requestPath);
    url.setQuery(query);
    const auto launch = [this, executable, url, requestPath] {
        if (!startVisualStudioCodeDetached(executable, {QStringLiteral("--open-url"), url.toString(QUrl::FullyEncoded)})) {
            QFile::remove(requestPath);
            KMessageBox::error(this, i18n("Visual Studio Code could not be opened."));
        }
    };
    static bool companionReady = false;
    if (companionReady) {
        launch();
        return;
    }
    const QString packagePath = QDir(cache).filePath(QStringLiteral("thoughts-source-selection-0.2.0.vsix"));
    QFile package(QStringLiteral(":/thoughts/source-selection.vsix"));
    QSaveFile output(packagePath);
    if (!package.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
        QFile::remove(requestPath);
        KMessageBox::error(this, i18n("Could not prepare the VS Code source selection extension."));
        return;
    }
    const QByteArray packageBytes = package.readAll();
    if (output.write(packageBytes) != packageBytes.size() || !output.commit()) {
        QFile::remove(requestPath);
        KMessageBox::error(this, i18n("Could not save the VS Code source selection extension."));
        return;
    }
    // Install the bundled companion without blocking the reader UI.
    auto *installer = new QProcess(this);
    auto *timeout = new QTimer(installer);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, installer, [installer] { installer->kill(); });
    connect(installer, &QProcess::errorOccurred, this, [this, installer, requestPath](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            QFile::remove(requestPath);
            KMessageBox::error(this, i18n("Could not start VS Code to enable source selection."));
            installer->deleteLater();
        }
    });
    connect(installer, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, installer, timeout, launch, requestPath](int code, QProcess::ExitStatus status) {
        timeout->stop();
        if (code == 0 && status == QProcess::NormalExit) {
            companionReady = true;
            launch();
        } else {
            QFile::remove(requestPath);
            KMessageBox::error(this, i18n("Could not enable the VS Code source selection extension.\n\n%1", QString::fromUtf8(installer->readAllStandardError())));
        }
        installer->deleteLater();
    });
    configureVisualStudioCode(installer, executable, {QStringLiteral("--install-extension"), packagePath});
    installer->start();
    timeout->start(60000);
}

void Shell::recordPartDeveloperEvent(const QString &event, const QJsonObject &metadata)
{
    // Only the reader's own timing events; identifiers are salted like other ids.
    if (!event.startsWith(QLatin1String("proposal.")) && !event.startsWith(QLatin1String("read_aloud."))) {
        return;
    }
    QJsonObject data = metadata;
    if (data.contains(QStringLiteral("proposal_id"))) {
        data.insert(QStringLiteral("proposal_id"), Thoughts::DeveloperLog::documentId(data.value(QStringLiteral("proposal_id")).toString()));
    }
    Thoughts::DeveloperLog::record(event, data);
}

void Shell::recordUnplacedCommentOpened(const QString &id, int page)
{
    Thoughts::DeveloperLog::record(QStringLiteral("comment.unplaced.shown"), {{QStringLiteral("comment_id"), Thoughts::DeveloperLog::documentId(id)},
        {QStringLiteral("page"), page}});
}

void Shell::showCommentInOverview(const QString &pdfPath, const QString &annotationId, int page)
{
    Q_UNUSED(page)
    Thoughts::CommentsOverview *overview = commentsOverview();
    s_commentsOverviewShell = this;
    if (!overview->revealComment(pdfPath, annotationId)) {
        KMessageBox::error(this, i18n("The comment could not be found in Overview."), i18n("Overview"));
        return;
    }
    overview->setAttribute(Qt::WA_ShowWithoutActivating, false);
    overview->show();
    overview->raise();
    overview->activateWindow();
}

void Shell::trackPdf()
{
    const QDir docsRoot(ShellUtils::docsRoot());
    const QString currentPdfsPath = docsRoot.filePath(QStringLiteral("current_pdfs"));
    if (!QDir().mkpath(currentPdfsPath)) {
        KMessageBox::error(this, i18n("The tracked-PDF folder could not be created at %1.", QDir::toNativeSeparators(currentPdfsPath)), i18n("Track PDF"));
        return;
    }

    QString initialDirectory = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QString sourcePath;
    QString suggestedFilename;
    const int activeTab = m_tabWidget->currentIndex();
    QPointer<KParts::ReadWritePart> sourcePart = activeTab >= 0 && activeTab < m_tabs.size() ? m_tabs.at(activeTab).part : nullptr;
    QUrl originalUrl;
    uint page = 0;
    if (!m_forcePdfLibraryChooser && sourcePart) {
        sourcePath = activePdfSourcePath();
        suggestedFilename = activePdfSuggestedFilename();
        originalUrl = sourcePart->url();
        if (originalUrl.isLocalFile()) {
            initialDirectory = QFileInfo(originalUrl.toLocalFile()).absolutePath();
        }
        QMetaObject::invokeMethod(sourcePart, "currentPage", Qt::DirectConnection, Q_RETURN_ARG(uint, page));
    }
    if (sourcePath.isEmpty()) {
        sourcePath = QFileDialog::getOpenFileName(this, i18n("Choose a PDF for the Library"), initialDirectory, i18n("PDF documents (*.pdf)"));
        suggestedFilename = pdfLinkSafeFilename(QFileInfo(sourcePath).fileName());
        originalUrl = {};
        page = 0;
    }
    if (sourcePath.isEmpty()) {
        return;
    }

    if (!m_forcePdfLibraryChooser && sourcePart && sourcePart->isModified() && !sourcePart->queryClose()) {
        return;
    }

    const QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.isFile() || !sourceInfo.isReadable() || !readablePdfPayload(sourceInfo.absoluteFilePath())) {
        KMessageBox::error(this, i18n("Please choose a readable PDF file."), i18n("Track PDF"));
        return;
    }

    const QString normalizedCurrentPdfsPath = QFileInfo(currentPdfsPath).absoluteFilePath();
    if (sourceInfo.absolutePath() == normalizedCurrentPdfsPath) {
        const LocalLibraryEntry entry = localLibraryEntry(docsRoot.absolutePath(), sourceInfo.fileName());
        if (!entry.available || !entry.found) {
            KMessageBox::error(this, i18n("This canonical PDF is not safely identified by the local Library index."), i18n("Add PDF to Library"));
            return;
        }
        openUrl(QUrl::fromLocalFile(sourceInfo.absoluteFilePath()));
        return;
    }

    beginLibraryImport(sourceInfo.absoluteFilePath(), suggestedFilename.isEmpty() ? pdfLinkSafeFilename(sourceInfo.fileName()) : suggestedFilename, sourcePart, page, originalUrl);
}

void Shell::beginLibraryImport(const QString &sourcePath, const QString &filename, const QPointer<KParts::ReadWritePart> &sourcePart, uint page, const QUrl &originalUrl)
{
    if (!m_pendingLibraryImportId.isEmpty()) {
        KMessageBox::information(this, i18n("Another PDF is already being added to the Library."), i18n("Add PDF to Library"));
        return;
    }
    if (!sourcePart || !QFileInfo(sourcePath).isReadable() || !readablePdfPayload(sourcePath)) {
        KMessageBox::error(this, i18n("The open PDF is no longer available."), i18n("Add PDF to Library"));
        return;
    }

    const QDir docsRoot(ShellUtils::docsRoot());
    const QString safeFilename = pdfLinkSafeFilename(filename);
    const QString destinationPath = docsRoot.filePath(QStringLiteral("current_pdfs/%1").arg(safeFilename));
    const QString stem = QFileInfo(safeFilename).completeBaseName();
    const QString existingSidecar = caseInsensitiveFile(QDir(docsRoot.filePath(QStringLiteral("comments"))), stem + QStringLiteral("_comments.jsonl"));
    const QString existingLayout = caseInsensitiveFile(QDir(docsRoot.filePath(QStringLiteral("comments/.layout"))), stem + QStringLiteral(".json"));
    const QString archivedSidecar = newestArchivedSidecar(docsRoot.absolutePath(), stem);
    if (!QFileInfo::exists(destinationPath) && (!existingSidecar.isEmpty() || !existingLayout.isEmpty() || !archivedSidecar.isEmpty())) {
        QStringList historyKinds;
        if (!existingSidecar.isEmpty()) {
            historyKinds.append(i18n("an orphaned comment history"));
        }
        if (!existingLayout.isEmpty()) {
            historyKinds.append(i18n("source-link metadata"));
        }
        if (!archivedSidecar.isEmpty()) {
            historyKinds.append(i18n("an archived comment history"));
        }
        const int choice = KMessageBox::warningTwoActionsCancel(this,
                                                                i18n("The name %1 already has %2. Reuse that history for this PDF?", stem, historyKinds.join(i18n(" and "))),
                                                                i18n("Existing PDF History"),
                                                                KGuiItem(i18n("Reuse and Add"), QStringLiteral("document-import")),
                                                                KGuiItem(i18n("Choose Another Name"), QStringLiteral("edit-rename")));
        if (choice == KMessageBox::SecondaryAction) {
            bool accepted = false;
            const QString requested = QInputDialog::getText(this, i18n("Choose Another Library Name"), i18n("PDF name:"), QLineEdit::Normal, stem, &accepted);
            if (accepted && !requested.trimmed().isEmpty()) {
                beginLibraryImport(sourcePath, pdfLinkSafeFilename(requested), sourcePart, page, originalUrl);
            }
            return;
        }
        if (choice != KMessageBox::PrimaryAction) {
            return;
        }
    }

    m_pendingLibraryImportId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_pendingLibraryImportFilename = safeFilename;
    m_pendingLibraryImportSourcePath = sourcePath;
    m_pendingLibraryImportArchivedSidecar = existingSidecar.isEmpty() ? archivedSidecar : QString();
    m_pendingLibraryImportDestinationSidecar = existingSidecar.isEmpty() && !archivedSidecar.isEmpty() ? docsRoot.filePath(QStringLiteral("comments/%1_comments.jsonl").arg(stem)) : QString();
    m_pendingLibraryImportPart = sourcePart;
    m_pendingLibraryImportOriginalUrl = originalUrl;
    m_pendingLibraryImportPage = page;
    const bool invoked = QMetaObject::invokeMethod(sourcePart, "importLibraryPdf", Qt::DirectConnection, Q_ARG(QString, m_pendingLibraryImportId), Q_ARG(QString, sourcePath), Q_ARG(QString, safeFilename));
    if (!invoked) {
        m_pendingLibraryImportId.clear();
        m_pendingLibraryImportFilename.clear();
        m_pendingLibraryImportSourcePath.clear();
        m_pendingLibraryImportArchivedSidecar.clear();
        m_pendingLibraryImportDestinationSidecar.clear();
        m_pendingLibraryImportPart.clear();
        m_pendingLibraryImportOriginalUrl = {};
        m_pendingLibraryImportPage = 0;
        KMessageBox::error(this, i18n("Sync with the server is unavailable."), i18n("Add PDF to Library"));
    }
    updatePdfLibraryAction();
}

void Shell::handleLibraryImportFinished(const QString &requestId, const QString &filename, const QString &path, const QString &sha256, bool success, int statusCode, const QString &message)
{
    Q_UNUSED(sha256)
    if (requestId.isEmpty() || requestId != m_pendingLibraryImportId) {
        return;
    }
    const QString archivedSidecar = m_pendingLibraryImportArchivedSidecar;
    const QString destinationSidecar = m_pendingLibraryImportDestinationSidecar;
    const QString sourcePath = m_pendingLibraryImportSourcePath;
    const QPointer<KParts::ReadWritePart> sourcePart = m_pendingLibraryImportPart;
    const QUrl originalUrl = m_pendingLibraryImportOriginalUrl;
    const uint page = m_pendingLibraryImportPage;
    m_pendingLibraryImportId.clear();
    m_pendingLibraryImportFilename.clear();
    m_pendingLibraryImportSourcePath.clear();
    m_pendingLibraryImportArchivedSidecar.clear();
    m_pendingLibraryImportDestinationSidecar.clear();
    m_pendingLibraryImportPart.clear();
    m_pendingLibraryImportOriginalUrl = {};
    m_pendingLibraryImportPage = 0;
    m_pdfLibraryStateReady = false;
    updatePdfLibraryAction();
    if (!success) {
        if (statusCode == 409 && sourcePart && QFileInfo(sourcePath).isReadable()) {
            bool accepted = false;
            const QString requested = QInputDialog::getText(this, i18n("Library Name Conflict"), i18n("A different PDF already uses %1. Choose another name:", filename), QLineEdit::Normal, QFileInfo(filename).completeBaseName(), &accepted);
            if (accepted && !requested.trimmed().isEmpty()) {
                beginLibraryImport(sourcePath, pdfLinkSafeFilename(requested), sourcePart, page, originalUrl);
            }
            return;
        }
        KMessageBox::error(this, message, i18n("Add PDF to Library"));
        return;
    }

    QString error;
    if (!archivedSidecar.isEmpty() && !restoreArchivedSidecar(archivedSidecar, destinationSidecar, &error)) {
        KMessageBox::error(this, i18n("The PDF was added, but its archived comment history could not be restored: %1", error), i18n("Add PDF to Library"));
    }
    KParts::ReadWritePart *part = sourcePart;
    if (part && !originalUrl.isEmpty()) {
        if (part->url() != originalUrl || part->isModified()) {
            KMessageBox::information(this, i18n("The PDF was added, but the original tab changed while the upload was running. The canonical Library copy is available at %1.", QDir::toNativeSeparators(path)), i18n("Add PDF to Library"));
            part = nullptr;
        } else if (!part->openUrl(QUrl::fromLocalFile(path))) {
            KMessageBox::error(this, i18n("The PDF was added, but its canonical copy could not replace the current tab."), i18n("Add PDF to Library"));
            part = nullptr;
        } else if (page > 0) {
            QMetaObject::invokeMethod(part, "goToPage", Qt::QueuedConnection, Q_ARG(uint, page));
        }
    } else if (originalUrl.isEmpty()) {
        part = trackedCommentPart(path, true, &error);
    }
    if (!part) {
        if (!error.isEmpty()) {
            KMessageBox::error(this, i18n("The PDF was added, but its local copy could not be opened: %1", error), i18n("Add PDF to Library"));
        }
        if (s_commentsOverview) {
            s_commentsOverview->requestReload();
        }
        return;
    }
    QVariantMap syncResult;
    const bool synchronized = QMetaObject::invokeMethod(part, "synchronizeDocsCommentsNow", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, syncResult));
    if (!synchronized || !syncResult.value(QStringLiteral("success")).toBool()) {
        const QString detail = synchronized ? syncResult.value(QStringLiteral("error")).toString() : i18n("Comment synchronization is unavailable.");
        KMessageBox::error(this, i18n("The PDF was added, but its embedded comments could not be synchronized: %1", detail), i18n("Add PDF to Library"));
    }
    if (s_commentsOverview) {
        s_commentsOverview->requestReload();
    }
    updatePdfLibraryAction();
}

void Shell::choosePdfForLibrary()
{
    const int choice = KMessageBox::questionTwoActionsCancel(this,
                                                             i18n("How would you like to add a project to the shared computer and phone Library?"),
                                                             i18n("Add project"),
                                                             KGuiItem(i18n("Publish Source Document…"), QStringLiteral("document-send")),
                                                             KGuiItem(i18n("Add External PDF…"), QStringLiteral("document-import")));
    if (choice == KMessageBox::PrimaryAction) {
        publishSourceDocument();
    } else if (choice == KMessageBox::SecondaryAction) {
        chooseExternalPdfForLibrary();
    }
}

void Shell::chooseExternalPdfForLibrary()
{
    m_forcePdfLibraryChooser = true;
    trackPdf();
    m_forcePdfLibraryChooser = false;
}

void Shell::publishSourceDocument()
{
    if (!m_pendingSourceTarget.isEmpty()) {
        return;
    }
    const QString executable = Thoughts::Platform::nativeServerTool(QStringLiteral("thoughts-publish-sources"));
    if (!QFileInfo::exists(executable)) {
        KMessageBox::error(this, i18n("The source publisher installed with Thoughts is missing; reinstall Thoughts."), i18n("Publish Source Document"));
        return;
    }
    QProcess process;
    process.setProgram(executable);
    process.setArguments({QStringLiteral("list-candidates"), QStringLiteral("--docs-root"), ShellUtils::docsRoot()});
    process.start();
    if (!process.waitForStarted(3000) || !process.waitForFinished(10000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString detail = QString::fromUtf8(process.readAllStandardError()).trimmed();
        KMessageBox::error(this, detail.isEmpty() ? i18n("Eligible unpublished source documents could not be discovered.") : detail, i18n("Publish Source Document"));
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(process.readAllStandardOutput(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        KMessageBox::error(this, i18n("The source-publication helper returned an invalid candidate list."), i18n("Publish Source Document"));
        return;
    }
    const QJsonArray candidates = document.array();
    if (candidates.isEmpty()) {
        KMessageBox::information(this, i18n("Every eligible source document is already represented in the Library."), i18n("Publish Source Document"));
        return;
    }
    QStringList labels;
    for (const QJsonValue &value : candidates) {
        const QJsonObject candidate = value.toObject();
        labels.append(i18n("%1 → %2", candidate.value(QStringLiteral("project")).toString(), candidate.value(QStringLiteral("target")).toString()));
    }
    int selected = 0;
    if (labels.size() > 1) {
        bool accepted = false;
        const QString label = QInputDialog::getItem(this, i18n("Publish Source Document"), i18n("Source project:"), labels, 0, false, &accepted);
        if (!accepted) {
            return;
        }
        selected = labels.indexOf(label);
        if (selected < 0) {
            return;
        }
    }
    const QJsonObject candidate = candidates.at(selected).toObject();
    const QString project = candidate.value(QStringLiteral("project")).toString();
    const QString target = candidate.value(QStringLiteral("target")).toString();
    if (project.isEmpty() || target.isEmpty()) {
        KMessageBox::error(this, i18n("The selected source-publication candidate is invalid."), i18n("Publish Source Document"));
        return;
    }
    if (KMessageBox::questionTwoActions(this,
                                        i18n("Publish %1 as %2?\n\nThoughts will prepare the exact local inputs, publish them to the server, build the PDF there, and then activate the complete authoritative Library generation. The current "
                                             "Library remains active if any phase fails.",
                                             project,
                                             target),
                                        i18n("Publish Source Document"),
                                        KGuiItem(i18n("Publish and Build"), QStringLiteral("document-send")),
                                        KStandardGuiItem::cancel()) != KMessageBox::PrimaryAction) {
        return;
    }
    const int activeTab = m_tabWidget ? m_tabWidget->currentIndex() : -1;
    if (activeTab < 0 || activeTab >= m_tabs.size()) {
        KMessageBox::error(this, i18n("Sync with the server is unavailable."), i18n("Publish Source Document"));
        return;
    }
    m_pendingSourceProject = project;
    m_pendingSourceTarget = target;
    m_activeBuildOperationId = beginSyncOperation(QStringLiteral("build"), target, QStringLiteral("preflight"), QStringLiteral("publish_source_document"));
    m_activeBuildOperationTarget = target;
    m_cachedFedoraRenderTarget = target;
    m_cachedFedoraRenderStatus = {{QStringLiteral("status"), QStringLiteral("preparing")}};
    if (!QMetaObject::invokeMethod(m_tabs.at(activeTab).part, "publishNewSourceDocument", Qt::DirectConnection, Q_ARG(QString, project), Q_ARG(QString, target), Q_ARG(QString, m_activeBuildOperationId))) {
        updateSyncOperation(m_activeBuildOperationId, Thoughts::SyncOperationState::Failed, QStringLiteral("failed"), i18n("Build could not be started"));
        m_activeBuildOperationId.clear();
        m_activeBuildOperationTarget.clear();
        m_pendingSourceProject.clear();
        m_pendingSourceTarget.clear();
        KMessageBox::error(this, i18n("Sync with the server is unavailable."), i18n("Publish Source Document"));
        return;
    }
    updatePdfLibraryAction();
}

void Shell::openPendingSourcePublicationOutput()
{
    if (m_pendingSourceTarget.isEmpty()) {
        return;
    }
    const int activeTab = m_tabWidget ? m_tabWidget->currentIndex() : -1;
    if (activeTab < 0 || activeTab >= m_tabs.size()) {
        return;
    }
    QVariantMap activity;
    QMetaObject::invokeMethod(m_tabs.at(activeTab).part, "fedoraBuildActivity", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, activity), Q_ARG(QString, m_pendingSourceTarget));
    if (!activity.value(QStringLiteral("active")).toBool()) {
        return;
    }
    QString error;
    if (!m_buildTerminalLauncher ||
        !m_buildTerminalLauncher->open(activity.value(QStringLiteral("run_directory")).toString(), activity.value(QStringLiteral("watcher")).toString(), activity.value(QStringLiteral("server")).toString(), &error)) {
        KMessageBox::error(this, error.isEmpty() ? i18n("The server build output could not be opened.") : error, i18n("Build Output"));
    }
}

void Shell::openSourcePublicationLog()
{
    KParts::ReadWritePart *part = fedoraGatewayPart();
    if (!part) {
        return;
    }
    QVariantMap activity;
    QMetaObject::invokeMethod(part, "fedoraSourcePublicationActivity", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, activity));
    QString error;
    if (!m_buildTerminalLauncher || !m_buildTerminalLauncher->openLog(activity.value(QStringLiteral("run_directory")).toString(), activity.value(QStringLiteral("log_path")).toString(), &error)) {
        KMessageBox::error(s_commentsOverview ? static_cast<QWidget *>(s_commentsOverview.data()) : this, error.isEmpty() ? i18n("The Sync log could not be opened.") : error, i18n("Sync Log"));
    }
}

void Shell::renameUnlinkedDocument(const QString &filename, const QString &sha256, const QString &displayName)
{
    const LocalLibraryEntry entry = localLibraryEntry(ShellUtils::docsRoot(), filename);
    if (!entry.available || !entry.found || (entry.kind != QLatin1String("source_linked") && entry.kind != QLatin1String("unlinked")) || entry.sha256 != sha256) {
        KMessageBox::error(s_commentsOverview ? static_cast<QWidget *>(s_commentsOverview.data()) : this, i18n("This document is no longer a Library PDF with the expected identity."), i18n("Rename Document"));
        return;
    }
    if (!m_pendingLibraryDisplayNameId.isEmpty()) {
        KMessageBox::information(s_commentsOverview ? static_cast<QWidget *>(s_commentsOverview.data()) : this, i18n("Another Library document is already being renamed."), i18n("Rename Document"));
        return;
    }
    const int activeTab = m_tabWidget ? m_tabWidget->currentIndex() : -1;
    if (activeTab < 0 || activeTab >= m_tabs.size()) {
        KMessageBox::error(s_commentsOverview ? static_cast<QWidget *>(s_commentsOverview.data()) : this, i18n("Sync with the server is unavailable."), i18n("Rename Document"));
        return;
    }
    m_pendingLibraryDisplayNameId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const bool invoked =
        QMetaObject::invokeMethod(m_tabs.at(activeTab).part, "setLibraryDisplayName", Qt::DirectConnection, Q_ARG(QString, m_pendingLibraryDisplayNameId), Q_ARG(QString, filename), Q_ARG(QString, sha256), Q_ARG(QString, displayName));
    if (!invoked) {
        m_pendingLibraryDisplayNameId.clear();
        KMessageBox::error(s_commentsOverview ? static_cast<QWidget *>(s_commentsOverview.data()) : this, i18n("Sync with the server is unavailable."), i18n("Rename Document"));
    }
}

void Shell::handleLibraryDisplayNameFinished(const QString &requestId, const QString &filename, const QString &sha256, const QString &displayName, bool success, const QString &message)
{
    Q_UNUSED(filename)
    Q_UNUSED(sha256)
    Q_UNUSED(displayName)
    if (requestId.isEmpty() || requestId != m_pendingLibraryDisplayNameId) {
        return;
    }
    m_pendingLibraryDisplayNameId.clear();
    m_pdfLibraryStateReady = false;
    if (!success) {
        KMessageBox::error(s_commentsOverview ? static_cast<QWidget *>(s_commentsOverview.data()) : this, message, i18n("Rename Document"));
        return;
    }
    if (s_commentsOverview) {
        s_commentsOverview->requestReload();
    }
}

void Shell::showCommentsOverviewOnStartup()
{
    QList<Shell *> emptyShells;
    for (KMainWindow *window : KMainWindow::memberList()) {
        auto *shell = qobject_cast<Shell *>(window);
        if (!shell) {
            continue;
        }
        if (shell->hasOpenDocument()) {
            return;
        }
        if (shell->isVisible()) {
            emptyShells.append(shell);
        }
    }

    if (emptyShells.isEmpty()) {
        return;
    }

    Shell *homeShell = emptyShells.constFirst();
    homeShell->showOverviewHome();
    for (Shell *shell : std::as_const(emptyShells)) {
        if (shell != homeShell) {
            shell->hide();
        }
    }
}

void Shell::showWindowOnReactivation()
{
    // Closing the Library hides its PDF window too, so Thoughts keeps running with nothing on screen.
    const auto widgets = QApplication::topLevelWidgets();
    for (const QWidget *widget : widgets) {
        const Qt::WindowType type = widget->windowType();
        if (widget->isVisible() && (type == Qt::Window || type == Qt::Dialog || type == Qt::Sheet)) {
            return;
        }
    }
    for (KMainWindow *window : KMainWindow::memberList()) {
        auto *shell = qobject_cast<Shell *>(window);
        if (shell && shell->hasOpenDocument()) {
            shell->show();
            shell->raise();
            shell->activateWindow();
            return;
        }
    }
    if (Shell *shell = shellForCommentsOverview()) {
        shell->showCommentsOverview(true);
    }
}

KParts::ReadWritePart *Shell::trackedCommentPart(const QString &pdfPath, bool allowOpen, QString *error)
{
    const QFileInfo pdfInfo(pdfPath);
    if (!pdfInfo.isFile()) {
        if (error) {
            *error = i18n("The canonical PDF no longer exists: %1", QDir::toNativeSeparators(pdfPath));
        }
        return nullptr;
    }

    const QUrl url = QUrl::fromLocalFile(pdfInfo.absoluteFilePath());
    int tab = findTabIndex(url);
    if (tab >= 0) {
        setActiveTab(tab);
    } else if (allowOpen) {
        const int activeTab = m_tabWidget->currentIndex();
        if (activeTab >= 0 && activeTab < m_tabs.size() && m_tabs.at(activeTab).part->url().isEmpty()) {
            openUrl(url);
        } else {
            openNewTab(url, {});
        }
        tab = findTabIndex(url);
    }

    if (tab < 0 || tab >= m_tabs.size()) {
        if (error) {
            *error = allowOpen ? i18n("The PDF could not be opened: %1", QDir::toNativeSeparators(pdfPath)) : i18n("The PDF tab containing this Overview change is no longer open.");
        }
        return nullptr;
    }
    return m_tabs.at(tab).part;
}

void Shell::openTrackedDocument(const QString &pdfPath)
{
    QString error;
    if (!trackedCommentPart(pdfPath, true, &error)) {
        KMessageBox::error(s_commentsOverview ? static_cast<QWidget *>(s_commentsOverview.data()) : this, error);
        return;
    }
    show();
    raise();
    activateWindow();
}

void Shell::openTrackedComment(const QString &pdfPath, const QString &annotationId, int page)
{
    Thoughts::DeveloperLog::record(QStringLiteral("comment.open.requested"), {{QStringLiteral("document_id"), Thoughts::DeveloperLog::documentId(pdfPath)},
        {QStringLiteral("comment_id"), Thoughts::DeveloperLog::documentId(annotationId)}, {QStringLiteral("page"), page}});
    QString error;
    KParts::ReadWritePart *part = trackedCommentPart(pdfPath, true, &error);
    if (!part) {
        KMessageBox::error(s_commentsOverview ? static_cast<QWidget *>(s_commentsOverview.data()) : this, error);
        return;
    }

    show();
    raise();
    activateWindow();
    const quint64 requestGeneration = ++m_trackedCommentOpenGeneration;
    tryOpenTrackedComment(pdfPath, annotationId, page, requestGeneration, 0);
}

void Shell::openTrackedFeedback(const QString &pdfPath, const QString &feedbackId)
{
    QString error;
    if (!trackedCommentPart(pdfPath, true, &error)) {
        KMessageBox::error(s_commentsOverview ? static_cast<QWidget *>(s_commentsOverview.data()) : this, error);
        return;
    }
    show();
    raise();
    activateWindow();
    tryOpenTrackedFeedback(pdfPath, feedbackId, ++m_trackedCommentOpenGeneration, 0);
}

void Shell::tryOpenTrackedFeedback(const QString &pdfPath, const QString &feedbackId, quint64 requestGeneration, int attempt)
{
    if (requestGeneration != m_trackedCommentOpenGeneration) {
        return;
    }
    // A document just opened knows its comments a moment later.
    bool opened = false;
    QString error;
    if (KParts::ReadWritePart *part = trackedCommentPart(pdfPath, false, &error)) {
        QMetaObject::invokeMethod(part, "openDocumentFeedback", Qt::DirectConnection, Q_RETURN_ARG(bool, opened), Q_ARG(QString, feedbackId));
    }
    if (!opened && attempt < 40) {
        QTimer::singleShot(50, this, [this, pdfPath, feedbackId, requestGeneration, attempt] { tryOpenTrackedFeedback(pdfPath, feedbackId, requestGeneration, attempt + 1); });
    }
}

void Shell::openTrackedLogicalComment(const QString &pdfPath, const QString &commentId, int page, const QVariantMap &location)
{
    QString error;
    KParts::ReadWritePart *part = trackedCommentPart(pdfPath, true, &error);
    if (!part) {
        KMessageBox::error(s_commentsOverview ? static_cast<QWidget *>(s_commentsOverview.data()) : this, error);
        return;
    }
    show();
    raise();
    activateWindow();
    const quint64 requestGeneration = ++m_trackedCommentOpenGeneration;
    tryOpenTrackedLogicalComment(pdfPath, commentId, page, location, requestGeneration, 0);
}

void Shell::openTrackedAgentProposal(const QString &pdfPath, const QString &proposalId, const QString &sourcePath, int sourceLine)
{
    QString error;
    KParts::ReadWritePart *part = trackedCommentPart(pdfPath, true, &error);
    if (!part) {
        KMessageBox::error(s_commentsOverview ? static_cast<QWidget *>(s_commentsOverview.data()) : this, error.isEmpty() ? i18n("The proposal review could not be opened in the PDF.") : error);
        return;
    }

    show();
    raise();
    activateWindow();
    const quint64 requestGeneration = ++m_trackedCommentOpenGeneration;
    tryOpenTrackedAgentProposal(pdfPath, proposalId, sourcePath, sourceLine, requestGeneration, 0);
}

void Shell::tryOpenTrackedComment(const QString &pdfPath, const QString &annotationId, int page, quint64 requestGeneration, int attempt)
{
    if (requestGeneration != m_trackedCommentOpenGeneration) {
        return;
    }
    QString error;
    KParts::ReadWritePart *part = trackedCommentPart(pdfPath, false, &error);
    if (!part) {
        if (attempt < 40) {
            QTimer::singleShot(50, this, [this, pdfPath, annotationId, page, requestGeneration, attempt] { tryOpenTrackedComment(pdfPath, annotationId, page, requestGeneration, attempt + 1); });
            return;
        }
        KMessageBox::error(s_commentsOverview ? static_cast<QWidget *>(s_commentsOverview.data()) : this, error);
        return;
    }

    bool opened = false;
    const bool invoked = QMetaObject::invokeMethod(part, "openReviewAnnotation", Qt::DirectConnection, Q_RETURN_ARG(bool, opened), Q_ARG(QString, annotationId), Q_ARG(int, page));
    if (invoked && opened) {
        Thoughts::DeveloperLog::record(QStringLiteral("comment.open.accepted"), {{QStringLiteral("comment_id"), Thoughts::DeveloperLog::documentId(annotationId)},
            {QStringLiteral("attempt"), attempt}, {QStringLiteral("request_generation"), qint64(requestGeneration)}});
        return;
    }
    Thoughts::DeveloperLog::record(QStringLiteral("comment.open.retry"), {{QStringLiteral("comment_id"), Thoughts::DeveloperLog::documentId(annotationId)},
        {QStringLiteral("attempt"), attempt}, {QStringLiteral("invoked"), invoked}, {QStringLiteral("request_generation"), qint64(requestGeneration)}});
    if (attempt < 40) {
        QTimer::singleShot(50, this, [this, pdfPath, annotationId, page, requestGeneration, attempt] { tryOpenTrackedComment(pdfPath, annotationId, page, requestGeneration, attempt + 1); });
        return;
    }
    if (!invoked || !opened) {
        KMessageBox::error(s_commentsOverview ? static_cast<QWidget *>(s_commentsOverview.data()) : this,
                           i18n("The comment is not available in the loaded projection for %1. Synchronization will continue without discarding the request.", QFileInfo(pdfPath).fileName()));
        return;
    }
}

void Shell::tryOpenTrackedLogicalComment(const QString &pdfPath, const QString &commentId, int page, const QVariantMap &location, quint64 requestGeneration, int attempt)
{
    if (requestGeneration != m_trackedCommentOpenGeneration) {
        return;
    }
    QString error;
    KParts::ReadWritePart *part = trackedCommentPart(pdfPath, false, &error);
    if (!part) {
        if (attempt < 40) {
            QTimer::singleShot(50, this, [this, pdfPath, commentId, page, location, requestGeneration, attempt] { tryOpenTrackedLogicalComment(pdfPath, commentId, page, location, requestGeneration, attempt + 1); });
            return;
        }
    } else {
        QVariantMap result;
        const bool invoked = QMetaObject::invokeMethod(part, "openLogicalCommentLocation", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, result), Q_ARG(QString, commentId), Q_ARG(int, page), Q_ARG(QVariantMap, location));
        if (invoked && result.value(QStringLiteral("opened")).toBool()) {
            return;
        }
        error = invoked ? result.value(QStringLiteral("error")).toString() : i18n("The logical comment navigator is unavailable.");
        if (attempt < 40 && error.contains(i18n("not available"), Qt::CaseInsensitive)) {
            QTimer::singleShot(50, this, [this, pdfPath, commentId, page, location, requestGeneration, attempt] { tryOpenTrackedLogicalComment(pdfPath, commentId, page, location, requestGeneration, attempt + 1); });
            return;
        }
    }
    KMessageBox::error(s_commentsOverview ? static_cast<QWidget *>(s_commentsOverview.data()) : this, error.isEmpty() ? i18n("The logical comment location could not be opened in this PDF revision.") : error);
}

void Shell::tryOpenTrackedAgentProposal(const QString &pdfPath, const QString &proposalId, const QString &sourcePath, int sourceLine, quint64 requestGeneration, int attempt)
{
    if (requestGeneration != m_trackedCommentOpenGeneration) {
        return;
    }
    QString error;
    KParts::ReadWritePart *part = trackedCommentPart(pdfPath, false, &error);
    if (!part) {
        if (attempt < 40) {
            QTimer::singleShot(50, this, [this, pdfPath, proposalId, sourcePath, sourceLine, requestGeneration, attempt] { tryOpenTrackedAgentProposal(pdfPath, proposalId, sourcePath, sourceLine, requestGeneration, attempt + 1); });
            return;
        }
    } else {
        QVariantMap result;
        const bool invoked = QMetaObject::invokeMethod(part, "openAgentProposalReview", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, result), Q_ARG(QString, proposalId), Q_ARG(QString, sourcePath), Q_ARG(int, sourceLine));
        if (invoked && result.value(QStringLiteral("opened")).toBool()) {
            return;
        }
        if (invoked && result.value(QStringLiteral("pending")).toBool() && attempt < 40) {
            QTimer::singleShot(50, this, [this, pdfPath, proposalId, sourcePath, sourceLine, requestGeneration, attempt] { tryOpenTrackedAgentProposal(pdfPath, proposalId, sourcePath, sourceLine, requestGeneration, attempt + 1); });
            return;
        }
        error = result.value(QStringLiteral("error")).toString();
    }
    KMessageBox::error(s_commentsOverview ? static_cast<QWidget *>(s_commentsOverview.data()) : this, error.isEmpty() ? i18n("The proposal review could not be opened in %1.", QFileInfo(pdfPath).fileName()) : error);
}

void Shell::editTrackedComment(const QString &pdfPath, const QString &annotationId, int page, const QString &contents, const QVariantMap &metadataPatch)
{
    QString error;
    KParts::ReadWritePart *part = trackedCommentPart(pdfPath, true, &error);
    QVariantMap result;
    if (part) {
        const bool invoked =
            QMetaObject::invokeMethod(part, "editReviewAnnotation", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, result), Q_ARG(QString, annotationId), Q_ARG(int, page), Q_ARG(QString, contents), Q_ARG(QVariantMap, metadataPatch));
        if (!invoked) {
            error = i18n("Thoughts could not invoke the comment editor.");
        }
    }
    if (!error.isEmpty()) {
        result = {{QStringLiteral("success"), false}, {QStringLiteral("partial"), false}, {QStringLiteral("error"), error}};
    }
    if (s_commentsOverview) {
        s_commentsOverview->completeOperation(Thoughts::CommentsOverview::EditOperation, pdfPath, annotationId, result);
        s_commentsOverview->show();
        s_commentsOverview->raise();
        s_commentsOverview->activateWindow();
    }
}

void Shell::replyToTrackedComment(const QString &pdfPath, const QString &parentAnnotationId, int page, const QString &contents, const QVariantMap &logicalPayload)
{
    QVariantMap result;
    QString error;
    if (!logicalPayload.isEmpty()) {
        bool queued = false;
        if (KParts::ReadWritePart *part = fedoraGatewayPart()) {
            QMetaObject::invokeMethod(part, "enqueueLogicalCommentMutation", Qt::DirectConnection, Q_RETURN_ARG(bool, queued), Q_ARG(QVariantMap, logicalPayload));
        }
        result = {
            {QStringLiteral("success"), queued},
            {QStringLiteral("changed"), queued},
            {QStringLiteral("replyId"), logicalPayload.value(QStringLiteral("comment_id"))},
            {QStringLiteral("error"), queued ? QString() : i18n("The global reply could not be added to the durable sync queue.")},
        };
    } else {
        KParts::ReadWritePart *part = trackedCommentPart(pdfPath, true, &error);
        if (part) {
            const bool invoked = QMetaObject::invokeMethod(part,
                                                           "addReviewAnnotationReply",
                                                           Qt::DirectConnection,
                                                           Q_RETURN_ARG(QVariantMap, result),
                                                           Q_ARG(QString, parentAnnotationId),
                                                           Q_ARG(int, page),
                                                           Q_ARG(QString, contents));
            if (!invoked) {
                error = i18n("Thoughts could not invoke the comment reply operation.");
            }
        }
        if (!error.isEmpty()) {
            result = {{QStringLiteral("success"), false}, {QStringLiteral("error"), error}};
        }
    }
    if (s_commentsOverview) {
        s_commentsOverview->completeReply(pdfPath, parentAnnotationId, result);
        s_commentsOverview->show();
        s_commentsOverview->raise();
        s_commentsOverview->activateWindow();
    }
}

void Shell::deleteTrackedComments(const QList<Thoughts::TrackedCommentTarget> &targets)
{
    QList<Thoughts::TrackedCommentTarget> deleted;
    const QVariantMap result = applyTrackedCommentDeletion(targets, &deleted);
    if (s_commentsOverview) {
        s_commentsOverview->completeBatchDelete(deleted, result);
        s_commentsOverview->show();
        s_commentsOverview->raise();
        s_commentsOverview->activateWindow();
    }
}

QVariantMap Shell::applyTrackedCommentDeletion(const QList<Thoughts::TrackedCommentTarget> &targets, QList<Thoughts::TrackedCommentTarget> *deletedTargets)
{
    QList<Thoughts::TrackedCommentTarget> &deleted = *deletedTargets;
    QStringList errors;
    QStringList pdfOrder;
    QHash<QString, QList<Thoughts::TrackedCommentTarget>> targetsByPdf;
    const auto projections = effectiveFedoraProjections();
    for (const Thoughts::TrackedCommentTarget &target : targets) {
        const QString groupKey = projections.contains(QStringLiteral("logical_comment\ncomments\n") + target.annotationId) ? QStringLiteral("@logical") : target.pdfPath;
        if (!targetsByPdf.contains(groupKey)) pdfOrder.append(groupKey);
        targetsByPdf[groupKey].append(target);
    }

    bool incompleteSynchronization = false;
    for (const QString &pdfPath : std::as_const(pdfOrder)) {
        QString error;
        KParts::ReadWritePart *part = pdfPath == QLatin1String("@logical") || pdfPath.isEmpty() ? fedoraGatewayPart() : trackedCommentPart(pdfPath, true, &error);
        if (!part) {
            errors.append(i18n("%1: %2", QFileInfo(pdfPath).fileName(), error));
            continue;
        }

        QVariantList encodedTargets;
        const QList<Thoughts::TrackedCommentTarget> pdfTargets = targetsByPdf.value(pdfPath);
        encodedTargets.reserve(pdfTargets.size());
        for (const Thoughts::TrackedCommentTarget &target : pdfTargets) {
            encodedTargets.append(QVariantMap {
                {QStringLiteral("annotationId"), target.annotationId},
                {QStringLiteral("page"), target.page},
            });
        }
        QVariantMap itemResult;
        const bool invoked = QMetaObject::invokeMethod(part, "deleteReviewAnnotations", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, itemResult), Q_ARG(QVariantList, encodedTargets));
        const bool applied = invoked && (itemResult.value(QStringLiteral("success")).toBool() || itemResult.value(QStringLiteral("partial")).toBool());
        const bool changed = itemResult.value(QStringLiteral("changed"), applied).toBool();
        if (applied && changed) {
            for (auto target : pdfTargets) {
                target.serviceToken = itemResult.value(QStringLiteral("serviceToken")).toInt();
                target.serviceOwner = part;
                deleted.append(target);
            }
            incompleteSynchronization = incompleteSynchronization || itemResult.value(QStringLiteral("partial")).toBool();
        } else {
            const QString detail = invoked ? itemResult.value(QStringLiteral("error")).toString() : i18n("Thoughts could not invoke comment deletion.");
            errors.append(i18n("%1: %2", QFileInfo(pdfPath).fileName(), detail));
        }
        const QString warning = itemResult.value(QStringLiteral("error")).toString();
        if (applied && !warning.isEmpty()) {
            errors.append(i18n("%1: %2", QFileInfo(pdfPath).fileName(), warning));
        }
    }
    const bool complete = deleted.size() == targets.size() && !incompleteSynchronization && errors.isEmpty();
    return {
        {QStringLiteral("success"), complete},
        {QStringLiteral("partial"), !deleted.isEmpty() && !complete},
        {QStringLiteral("changed"), !deleted.isEmpty()},
        {QStringLiteral("error"), errors.join(QLatin1Char('\n'))},
    };
}

void Shell::undoTrackedCommentChange(const QString &pdfPath)
{
    QString error;
    KParts::ReadWritePart *part = trackedCommentPart(pdfPath, false, &error);
    QVariantMap result;
    if (part) {
        const bool invoked = QMetaObject::invokeMethod(part, "undoReviewChange", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, result));
        if (!invoked) {
            error = i18n("Thoughts could not undo the Overview change.");
        }
    }
    if (!error.isEmpty()) {
        result = {
            {QStringLiteral("success"), false},
            {QStringLiteral("partial"), false},
            {QStringLiteral("historyReset"), !part},
            {QStringLiteral("error"), error},
        };
    }
    if (s_commentsOverview) {
        s_commentsOverview->completeOperation(Thoughts::CommentsOverview::UndoOperation, pdfPath, QString(), result);
        s_commentsOverview->raise();
        s_commentsOverview->activateWindow();
    }
}

void Shell::runCommentDeletionHistory(const QList<Thoughts::TrackedCommentTarget> &targets, bool undo)
{
    QMap<QString, QList<Thoughts::TrackedCommentTarget>> groups;
    for (const auto &target : targets) {
        const QString key = target.serviceToken
            ? QString::number(reinterpret_cast<quintptr>(target.serviceOwner.data())) + QLatin1Char(':') + QString::number(target.serviceToken)
            : target.pdfPath;
        groups[key].append(target);
    }
    QStringList errors;
    int completed = 0;
    for (const auto &group : groups) {
        const auto &target = group.first();
        QString error;
        auto *part = target.serviceToken ? qobject_cast<KParts::ReadWritePart *>(target.serviceOwner.data())
            : target.pdfPath.isEmpty() ? fedoraGatewayPart() : trackedCommentPart(target.pdfPath, false, &error);
        if (!part) { errors.append(error.isEmpty() ? i18n("The comment history owner is no longer open.") : error); continue; }
        QVariantMap result;
        const bool invoked = target.serviceToken
            ? QMetaObject::invokeMethod(part, undo ? "undoCommentDeletion" : "redoCommentDeletion", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, result), Q_ARG(int, target.serviceToken))
            : QMetaObject::invokeMethod(part, undo ? "undoReviewChange" : "redoReviewChange", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, result));
        if (invoked && result.value(QStringLiteral("success")).toBool()) completed += group.size();
        else {
            const QString error = result.value(QStringLiteral("error")).toString();
            errors.append(error.isEmpty() ? i18n("The comment history operation failed.") : error);
        }
    }
    if (s_commentsOverview) s_commentsOverview->completeBatchHistoryOperation(
        undo ? Thoughts::CommentsOverview::UndoOperation : Thoughts::CommentsOverview::RedoOperation, targets,
        {{QStringLiteral("success"), completed == targets.size()}, {QStringLiteral("partial"), completed > 0 && completed != targets.size()}, {QStringLiteral("error"), errors.join(QLatin1Char('\n'))}});
}

void Shell::undoTrackedCommentChanges(const QList<Thoughts::TrackedCommentTarget> &targets)
{
    runCommentDeletionHistory(targets, true);
}

void Shell::redoTrackedCommentChange(const QString &pdfPath)
{
    QString error;
    KParts::ReadWritePart *part = trackedCommentPart(pdfPath, false, &error);
    QVariantMap result;
    if (part) {
        const bool invoked = QMetaObject::invokeMethod(part, "redoReviewChange", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, result));
        if (!invoked) {
            error = i18n("Thoughts could not redo the Overview change.");
        }
    }
    if (!error.isEmpty()) {
        result = {
            {QStringLiteral("success"), false},
            {QStringLiteral("partial"), false},
            {QStringLiteral("historyReset"), !part},
            {QStringLiteral("error"), error},
        };
    }
    if (s_commentsOverview) {
        s_commentsOverview->completeOperation(Thoughts::CommentsOverview::RedoOperation, pdfPath, QString(), result);
        s_commentsOverview->raise();
        s_commentsOverview->activateWindow();
    }
}

void Shell::redoTrackedCommentChanges(const QList<Thoughts::TrackedCommentTarget> &targets)
{
    runCommentDeletionHistory(targets, false);
}

void Shell::saveProperties(KConfigGroup &group)
{
    if (!m_isValid) { // part couldn't be loaded, nothing to save
        return;
    }

    // Gather lists of settings to preserve
    QStringList urls, titleDocumentIds, titleProjectIds, displayTitles, pages;
    for (const TabState &tab : std::as_const(m_tabs)) {
        urls.append(tab.part->url().url());
        titleDocumentIds.append(tab.part->property("thoughtsTitleDocumentId").toString());
        titleProjectIds.append(tab.part->property("thoughtsTitleProjectId").toString());
        displayTitles.append(tab.part->property("thoughtsTitleFallback").toString());
        pages.append(tab.part->property(HTML_ATTACHMENT_TAB_PROPERTY).toString());
    }
    group.writeEntry(SESSION_HTML_PAGES_KEY, pages);
    group.writeEntry("TitleDocumentIds", titleDocumentIds);
    group.writeEntry("TitleProjectIds", titleProjectIds);
    group.writeEntry("DisplayTitles", displayTitles);
    group.writePathEntry(SESSION_URL_KEY, urls);
    group.writeEntry(SESSION_TAB_KEY, m_tabWidget->currentIndex());
}

void Shell::readProperties(const KConfigGroup &group)
{
    // Reopen documents based on saved settings
    QStringList urls = group.readPathEntry(SESSION_URL_KEY, QStringList());
    int desiredTab = group.readEntry<int>(SESSION_TAB_KEY, 0);

    const auto documentIds = group.readEntry("TitleDocumentIds", QStringList());
    const auto projectIds = group.readEntry("TitleProjectIds", QStringList());
    const auto displayTitles = group.readEntry("DisplayTitles", QStringList());
    const auto pages = group.readEntry(SESSION_HTML_PAGES_KEY, QStringList());
    for (int index = 0; index < urls.size(); ++index) {
        const QUrl url(urls.at(index));
        // A page's tab comes after the documents, next to its own.
        if (!pages.value(index).isEmpty()) continue;
        openUrl(url);
        const int tab = findTabIndex(url);
        if (tab >= 0) selectReaderTitle(m_tabs[tab].part, documentIds.value(index), displayTitles.value(index), projectIds.value(index));
    }
    for (int index = 0; index < urls.size(); ++index) {
        if (pages.value(index).isEmpty()) continue;
        const QUrl url(urls.at(index));
        int beside = findTabIndex(url);
        while (beside >= 0 && beside + 1 < m_tabs.size() && isHtmlAttachmentTab(beside + 1)) ++beside;
        if (auto *part = showHtmlAttachmentTab(url, {{QStringLiteral("path"), pages.at(index)}}, beside)) {
            selectReaderTitle(part, documentIds.value(index), displayTitles.value(index), projectIds.value(index));
        }
    }

    if (desiredTab < m_tabs.size()) {
        setActiveTab(desiredTab);
    }
}

QUrl Shell::webDocumentUrl(const QString &input)
{
    const QUrl url(input.trimmed(), QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || (scheme != QLatin1String("http") && scheme != QLatin1String("https")) || url.host().isEmpty()) {
        return {};
    }
    return url;
}

void Shell::openFromUrl()
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("openFromUrlDialog"));
    dialog.setWindowTitle(i18n("Open from URL"));
    dialog.setMinimumWidth(520);

    auto *layout = new QVBoxLayout(&dialog);
    auto *formLayout = new QFormLayout;
    auto *urlEdit = new QLineEdit(&dialog);
    urlEdit->setObjectName(QStringLiteral("openFromUrlEdit"));
    urlEdit->setPlaceholderText(QStringLiteral("https://example.com/document.pdf"));
    formLayout->addRow(i18n("&URL:"), urlEdit);
    layout->addLayout(formLayout);

    auto *errorLabel = new QLabel(&dialog);
    errorLabel->setObjectName(QStringLiteral("openFromUrlError"));
    errorLabel->setWordWrap(true);
    errorLabel->setVisible(false);
    layout->addWidget(errorLabel);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, &dialog);
    buttons->setObjectName(QStringLiteral("openFromUrlButtons"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    QUrl selectedUrl;
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        selectedUrl = webDocumentUrl(urlEdit->text());
        if (selectedUrl.isEmpty()) {
            errorLabel->setText(i18n("Enter a valid HTTP or HTTPS URL, including the host name."));
            errorLabel->setVisible(true);
            urlEdit->setFocus();
            urlEdit->selectAll();
            return;
        }
        dialog.accept();
    });

    urlEdit->setFocus();
    if (dialog.exec() == QDialog::Accepted) {
        openUrl(selectedUrl);
    }
}

void Shell::fileOpen()
{
    // this slot is called whenever the File->Open menu is selected,
    // the Open shortcut is pressed (usually CTRL+O) or the Open toolbar
    // button is clicked
    const int activeTab = m_tabWidget->currentIndex();
    if (!m_fileformatsscanned) {
        const KDocumentViewer *const doc = qobject_cast<KDocumentViewer *>(m_tabs[activeTab].part);
        Q_ASSERT(doc);

        m_fileformats = doc->supportedMimeTypes();

        m_fileformatsscanned = true;
    }

    QUrl startDir;
    const KParts::ReadWritePart *const curPart = m_tabs[activeTab].part;
    if (curPart->url().isLocalFile()) {
        startDir = KIO::upUrl(curPart->url());
    }
    if (startDir.isEmpty() || (startDir == QUrl::fromLocalFile(QDir::rootPath()))) {
        startDir = QUrl::fromLocalFile(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    }

    QPointer<QFileDialog> dlg(new QFileDialog(this));
    dlg->setDirectoryUrl(startDir);
    dlg->setAcceptMode(QFileDialog::AcceptOpen);
    dlg->setOption(QFileDialog::HideNameFilterDetails, true);
    dlg->setFileMode(QFileDialog::ExistingFiles); // Allow selection of more than one file

    QMimeDatabase mimeDatabase;
    // Unfortunately non Plasma file dialogs don't support the "All supported files" when using
    // setMimeTypeFilters instead of setNameFilters, so for those use setNameFilters which is a bit
    // worse because doesn't show you pdf files named bla.blo when you say "show me the pdf files", but
    // that's solvable by choosing "All Files" and it's not that common while it's more convenient to
    // only get shown the files that the application can open by default instead of all of them
    const bool useMimeTypeFilters = qgetenv("XDG_CURRENT_DESKTOP").toLower() == "kde";
    if (useMimeTypeFilters) {
        QStringList mimetypes;
        for (const QString &mimeName : std::as_const(m_fileformats)) {
            QMimeType mimeType = mimeDatabase.mimeTypeForName(mimeName);
            mimetypes << mimeType.name();
        }
        mimetypes.prepend(QStringLiteral("application/octet-stream"));
        dlg->setMimeTypeFilters(mimetypes);
    } else {
        QSet<QString> globPatterns;
        QMap<QString, QStringList> namedGlobs;
        for (const QString &mimeName : std::as_const(m_fileformats)) {
            QMimeType mimeType = mimeDatabase.mimeTypeForName(mimeName);
            const QStringList globs(mimeType.globPatterns());
            if (globs.isEmpty()) {
                continue;
            }

            globPatterns.unite(QSet<QString>(globs.begin(), globs.end()));

            namedGlobs[mimeType.comment()].append(globs);
        }
        QStringList namePatterns;
        for (auto it = namedGlobs.cbegin(); it != namedGlobs.cend(); ++it) {
            namePatterns.append(it.key() + QLatin1String(" (") + it.value().join(QLatin1Char(' ')) + QLatin1Char(')'));
        }

        const QStringList allGlobPatterns = globPatterns.values();
        namePatterns.prepend(i18n("All files (*)"));
        namePatterns.prepend(i18n("All supported files (%1)", allGlobPatterns.join(QLatin1Char(' '))));
        dlg->setNameFilters(namePatterns);
    }

    dlg->setWindowTitle(i18n("Open Document")); /* cppcheck-suppress nullPointerRedundantCheck ; QPointer things here is not understood*/
    if (dlg->exec() && dlg) {                   /* cppcheck-suppress nullPointerRedundantCheck ; QPointer things here is not understood*/
        const QList<QUrl> urlList = dlg->selectedUrls();
        for (const QUrl &url : urlList) {
            openUrl(url);
        }
    }

    if (dlg) {
        delete dlg.data();
    }
}

void Shell::tryRaise(const QString &startupId)
{
#if !defined(Q_OS_WIN) && !defined(Q_OS_OSX) && !defined(Q_OS_HAIKU)
    if (KWindowSystem::isPlatformWayland()) {
        KWindowSystem::setCurrentXdgActivationToken(startupId);
    } else if (KWindowSystem::isPlatformX11()) {
        KStartupInfo::setNewStartupId(window()->windowHandle(), startupId.toUtf8());
    }
#else
    Q_UNUSED(startupId);
#endif

    KWindowSystem::activateWindow(window()->windowHandle());
}

// only called when starting the program
void Shell::setFullScreen(bool useFullScreen)
{
    if (useFullScreen) {
        setWindowState(windowState() | Qt::WindowFullScreen); // set
    } else {
        setWindowState(windowState() & ~Qt::WindowFullScreen); // reset
    }
}

void Shell::selectReaderTitle(KParts::ReadWritePart *part, const QString &documentId, const QString &title, const QString &projectId)
{
    part->setProperty("thoughtsTitleDocumentId", documentId);
    part->setProperty("thoughtsTitleProjectId", projectId);
    part->setProperty("thoughtsTitleFallback", title);
    part->setProperty("thoughtsTitleUrl", part->url());
    const auto identity = Thoughts::DocumentIdentityStore(ShellUtils::docsRoot()).forFile(part->url().toLocalFile());
    part->setProperty("thoughtsTitlePdfId", identity.value(QStringLiteral("document_id")).toString());
    refreshReaderTabTitles();
}

void Shell::refreshReaderTabTitles()
{
    for (int index = 0; index < m_tabs.size(); ++index) {
        auto *part = m_tabs[index].part;
        const auto previousUrl = part->property("thoughtsTitleUrl").toUrl();
        if (!previousUrl.isEmpty() && previousUrl.adjusted(QUrl::RemoveFragment) != part->url().adjusted(QUrl::RemoveFragment)) {
            const auto pdfId = part->property("thoughtsTitlePdfId").toString();
            const auto identity = Thoughts::DocumentIdentityStore(ShellUtils::docsRoot()).forFile(part->url().toLocalFile());
            if (pdfId.isEmpty() || identity.value(QStringLiteral("document_id")).toString() != pdfId) {
                part->setProperty("thoughtsTitleDocumentId", QVariant());
                part->setProperty("thoughtsTitleProjectId", QVariant());
                part->setProperty("thoughtsTitleFallback", QVariant());
            }
            part->setProperty("thoughtsTitleUrl", part->url());
        }
        QString fallback = part->property("thoughtsTitleFallback").toString();
        // An earlier version from a card's History keeps its title when its tab is restored.
        if (fallback.isEmpty() && part->url().isLocalFile() && Thoughts::PdfVersions::isCachedVersion(part->url().toLocalFile())) {
            fallback = Thoughts::PdfVersions::title(part->url().toLocalFile());
            if (fallback.isEmpty()) fallback = i18n("Earlier version of a PDF");
        }
        const QString title = m_readerDisplayTitles
            ? m_readerDisplayTitles->title(part->url(), part->property("thoughtsTitleDocumentId").toString(),
                part->property("thoughtsTitleProjectId").toString(), fallback)
            : fallback.isEmpty() ? part->url().fileName() : fallback;
        if (isHtmlAttachmentTab(index)) {
            // A page's tab: the page, from its document.
            QString page;
            QMetaObject::invokeMethod(part, "htmlAttachmentTitle", Qt::DirectConnection, Q_RETURN_ARG(QString, page));
            if (page.isEmpty()) page = QFileInfo(part->property(HTML_ATTACHMENT_TAB_PROPERTY).toString()).completeBaseName();
            m_tabWidget->setTabText(index, page);
            m_tabWidget->setTabToolTip(index, title.isEmpty() ? page : i18nc("tab tooltip: page title, document title", "%1\nA page of %2", page, title));
            if (index == m_tabWidget->currentIndex()) KParts::MainWindow::setCaption(page, false);
            continue;
        }
        const QString label = title + (part->isModified() && !title.isEmpty() ? QStringLiteral(" *") : QString());
        m_tabWidget->setTabText(index, label);
        m_tabWidget->setTabToolTip(index, title);
        if (index == m_tabWidget->currentIndex()) KParts::MainWindow::setCaption(title, part->isModified());
    }
}

void Shell::setCaption(const QString &caption)
{
    Q_UNUSED(caption)
    refreshReaderTabTitles();
}

void Shell::changeEvent(QEvent *event)
{
    KParts::MainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        styleMainToolBarActions();
    } else if (event->type() == QEvent::PaletteChange) {
        applyReaderIcons(actionCollection());
        for (const TabState &tab : std::as_const(m_tabs)) {
            applyReaderIcons(tab.part->actionCollection());
        }
        styleDocumentTabs();
        m_pdfSyncIconName.clear();
        updatePdfSyncStatus();
        styleMainToolBarActions();
    }
}

void Shell::showEvent(QShowEvent *e)
{
    Thoughts::ReaderChrome::hideWindowTitle(this);
    if (!menuBar()->isNativeMenuBar() && m_showMenuBarAction) {
        m_showMenuBarAction->setChecked(menuBar()->isVisible());
    }

    KParts::MainWindow::showEvent(e);
    Thoughts::ReaderChrome::alignWindowButtons(this);
    QTimer::singleShot(0, this, &Shell::syncPdfLibraryStatusToolButton);
}

void Shell::slotUpdateFullScreen()
{
    if (m_fullScreenAction->isChecked()) {
        m_menuBarWasShown = !menuBar()->isHidden();
        menuBar()->hide();

        m_toolBarWasShown = !toolBar()->isHidden();
        toolBar()->hide();

        KToggleFullScreenAction::setFullScreen(this, true);
    } else {
        if (m_menuBarWasShown) {
            menuBar()->show();
        }
        if (m_toolBarWasShown) {
            toolBar()->show();
        }
        KToggleFullScreenAction::setFullScreen(this, false);
    }
}

void Shell::slotShowMenubar()
{
    if (menuBar()->isHidden()) {
        menuBar()->show();
    } else {
        menuBar()->hide();
    }
}

QSize Shell::sizeHint() const
{
    const QSize baseSize = QApplication::primaryScreen()->availableSize() * 0.6;
    // Set an arbitrary yet sensible sane minimum size for very small screens;
    // for example we don't want people using 1366x768 screens to get a tiny
    // default window size of 820 x 460 which will elide most of the toolbar buttons.
    return baseSize.expandedTo(QSize(1000, 700));
}

bool Shell::queryClose()
{
    // Closing a window with several tabs needs no confirmation (it also blocked
    // scripted quits). Each part still asks about unsaved changes.
    for (int i = 0; i < m_tabs.size(); ++i) {
        KParts::ReadWritePart *const part = m_tabs[i].part;

        // To resolve confusion about multiple modified docs, switch to relevant tab
        if (part->isModified()) {
            setActiveTab(i);
        }

        if (!part->queryClose()) {
            return false;
        }
    }
    return true;
}

void Shell::setActiveTab(int tab)
{
    // setCurrentIndex() below emits currentChanged, which calls back here for the
    // same tab; that nested pass rebuilt the whole KXMLGUI (~150 ms) a second time.
    if (m_activatingTab == tab) {
        return;
    }
    const int previousActivation = std::exchange(m_activatingTab, tab);
    const auto finishActivation = qScopeGuard([this, previousActivation] { m_activatingTab = previousActivation; });
    KToolBar *mainToolBar = toolBar(QStringLiteral("mainToolBar"));
    if (mainToolBar) {
        mainToolBar->installEventFilter(this);
    }
    ScopedWidgetUpdatesBlocker toolbarUpdates(mainToolBar);

    if (m_showSidebarAction) {
        m_showSidebarAction->disconnect(m_sidebar);
    }

    m_tabWidget->setCurrentIndex(tab);

    // NOTE : createGUI(...) breaks the visibility of the sidebar, so we need
    // to save and restore it
    const bool isSidebarVisible = m_sidebar->isVisible();
    createGUI(m_tabs[tab].part);
    m_sidebar->setVisible(isSidebarVisible);

    QAction *openFromUrlAction = actionCollection()->action(QStringLiteral("open_from_url"));
    QMenu *fileMenu = factory() ? qobject_cast<QMenu *>(factory()->container(QStringLiteral("file"), nullptr)) : nullptr;
    // KXMLGUI recreates menus when the active part changes. Older customized
    // layouts can omit these actions even when their XML version is current.
    if (fileMenu) {
        for (const QString &name : {QStringLiteral("thoughts_storage"), QStringLiteral("thoughts_logs"), QStringLiteral("thoughts_developer_logs")}) {
            QAction *action = actionCollection()->action(name);
            if (action && !fileMenu->actions().contains(action)) {
                fileMenu->addAction(action);
            }
        }
    }
    if (fileMenu && openFromUrlAction) {
        fileMenu->removeAction(openFromUrlAction);
        QAction *const fileOpenAction = actionCollection()->action(KStandardAction::name(KStandardAction::Open));
        const QList<QAction *> fileActions = fileMenu->actions();
        const int fileOpenIndex = fileActions.indexOf(fileOpenAction);
        QAction *const before = fileOpenIndex >= 0 && fileOpenIndex + 1 < fileActions.size() ? fileActions.at(fileOpenIndex + 1) : nullptr;
        fileMenu->insertAction(before, openFromUrlAction);
    }

    m_activePartToolbar->setActivePart(m_tabs[tab].part->actionCollection());
    recordCurrentNavigationViewport(false);
    updatePdfLibraryAction();
    if (mainToolBar) {
        materializeMainToolBar();
        scheduleMainToolBarMaterialization();
        styleMainToolBarActions();
        QTimer::singleShot(0, this, &Shell::styleMainToolBarActions);
        QTimer::singleShot(100, this, &Shell::styleMainToolBarActions);
        scheduleMainToolBarCentering();
    }

    // dock KPart's sidebar if new and make it current
    Okular::ViewerInterface *iPart = qobject_cast<Okular::ViewerInterface *>(m_tabs[tab].part);
    Q_ASSERT(iPart);
    QWidget *sideContainer = iPart->getSideContainer();
    if (m_sidebar->indexOf(sideContainer) == -1) {
        m_sidebar->addWidget(sideContainer);
        if (m_sidebar->maximumWidth() > sideContainer->maximumWidth()) {
            m_sidebar->setMaximumWidth(sideContainer->maximumWidth());
        }
    }
    m_sidebar->setCurrentWidget(sideContainer);

    m_showSidebarAction = m_tabs[tab].part->actionCollection()->action(QStringLiteral("show_leftpanel"));
    Q_ASSERT(m_showSidebarAction);
    m_showSidebarAction->disconnect(m_sidebar);
    m_showSidebarAction->setChecked(m_sidebar->isVisibleTo(this));
    connect(m_showSidebarAction, &QAction::triggered, m_sidebar, &Sidebar::setVisible);

    m_printAction->setEnabled(m_tabs[tab].printEnabled);
    m_closeAction->setEnabled(m_tabs[tab].closeEnabled);
}

// Part KActionMenus need their active Part instance in the toolbar: a generic
// proxy would lose the menu.
static bool isPartMenuToolbarAction(const QString &name)
{
    return name == QLatin1StringView("sidebar_display") || name == QLatin1StringView("view_render_mode") || name == QLatin1StringView("view_highlights");
}

void Shell::materializeMainToolBar()
{
    KToolBar *mainToolBar = toolBar(QStringLiteral("mainToolBar"));
    if (!mainToolBar || !m_activePartToolbar || !m_tabWidget || m_mainToolBarMaterializationInProgress) {
        return;
    }

    const int activeTab = m_tabWidget->currentIndex();
    if (activeTab < 0 || activeTab >= m_tabs.size()) {
        return;
    }

    const std::optional<QList<MainToolBarItem>> configuredItems = configuredMainToolBarItems(domDocument());
    if (!configuredItems) {
        return;
    }

    ScopedWidgetUpdatesBlocker toolbarUpdates(mainToolBar);
    QScopedValueRollback<bool> materializationGuard(m_mainToolBarMaterializationInProgress, true);

    KActionCollection *activePartActions = m_tabs[activeTab].part->actionCollection();
    auto configuredAction = [this, activePartActions](const QString &name) -> QAction * {
        QAction *partAction = activePartActions->action(name);
        // KActionMenu and QWidgetAction need their active Part instance in the
        // toolbar. Generic proxies lose the menu or cannot own the one-use
        // widget while KXMLGUI is rebuilding the active Part.
        if (isPartMenuToolbarAction(name) || qobject_cast<QWidgetAction *>(partAction)) {
            return partAction;
        }
        if (QAction *shellAction = actionCollection()->action(name)) {
            return shellAction;
        }
        return partAction;
    };

    const QList<QAction *> currentActions = mainToolBar->actions();
    int currentIndex = 0;
    bool matchesConfiguration = true;
    for (const MainToolBarItem &item : *configuredItems) {
        QAction *expectedAction = nullptr;
        if (item.type == MainToolBarItemType::Action) {
            expectedAction = configuredAction(item.name);
            if (!expectedAction) {
                continue;
            }
        }
        if (currentIndex >= currentActions.size()) {
            matchesConfiguration = false;
            break;
        }

        QAction *currentAction = currentActions.at(currentIndex++);
        if (item.type == MainToolBarItemType::Action) {
            if (currentAction != expectedAction) {
                matchesConfiguration = false;
                break;
            }
            if (isPartMenuToolbarAction(item.name) || qobject_cast<QWidgetAction *>(expectedAction)) {
                QWidget *host = mainToolBar->widgetForAction(expectedAction);
                QWidget *defaultWidget = qobject_cast<QWidgetAction *>(expectedAction) ? qobject_cast<QWidgetAction *>(expectedAction)->defaultWidget() : nullptr;
                if (!host || (defaultWidget && host != defaultWidget && !host->isAncestorOf(defaultWidget))) {
                    matchesConfiguration = false;
                    break;
                }
            }
        } else if (item.type == MainToolBarItemType::Separator) {
            if (!currentAction->isSeparator()) {
                matchesConfiguration = false;
                break;
            }
        } else {
            QWidget *widget = mainToolBar->widgetForAction(currentAction);
            if (!widget || !widget->sizePolicy().expandingDirections().testFlag(Qt::Horizontal)) {
                matchesConfiguration = false;
                break;
            }
        }
    }
    matchesConfiguration = matchesConfiguration && currentIndex == currentActions.size();
    if (matchesConfiguration) {
        styleMainToolBarActions();
        scheduleMainToolBarCentering();
        return;
    }

    mainToolBar->clear();
    for (const QPointer<QAction> &layoutAction : std::as_const(m_materializedMainToolBarLayoutActions)) {
        delete layoutAction.data();
    }
    m_materializedMainToolBarLayoutActions.clear();

    for (const MainToolBarItem &item : *configuredItems) {
        if (item.type == MainToolBarItemType::Action) {
            if (QAction *action = configuredAction(item.name)) {
                mainToolBar->addAction(action);
            }
        } else if (item.type == MainToolBarItemType::Separator) {
            auto *separator = new QAction(mainToolBar);
            separator->setObjectName(item.name);
            separator->setSeparator(true);
            mainToolBar->addAction(separator);
            m_materializedMainToolBarLayoutActions.append(separator);
        } else {
            auto *spacer = new QWidget(mainToolBar);
            spacer->setObjectName(item.name);
            spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            QAction *spacerAction = mainToolBar->addWidget(spacer);
            spacerAction->setObjectName(item.name);
            m_materializedMainToolBarLayoutActions.append(spacerAction);
        }
    }

    styleMainToolBarActions();
    scheduleMainToolBarCentering();
}

void Shell::scheduleMainToolBarMaterialization()
{
    if (m_mainToolBarMaterializationScheduled || m_mainToolBarMaterializationInProgress) {
        return;
    }
    m_mainToolBarMaterializationScheduled = true;
    QTimer::singleShot(0, this, [this] {
        m_mainToolBarMaterializationScheduled = false;
        materializeMainToolBar();
        if (m_mainToolBarUpdatesSuppressed) {
            m_mainToolBarUpdatesSuppressed = false;
            if (KToolBar *mainToolBar = toolBar(QStringLiteral("mainToolBar"))) {
                mainToolBar->setUpdatesEnabled(true);
                mainToolBar->update();
            }
        }
    });
}

void Shell::styleMainToolBarActions()
{
    KToolBar *mainToolBar = toolBar(QStringLiteral("mainToolBar"));
    if (!mainToolBar) {
        return;
    }
    using namespace Thoughts::ReaderTheme;
    const QPalette colors = mainToolBar->palette();
    const QString toolbarStyle = QStringLiteral("QToolBar#mainToolBar { background: %1; border: 0; border-bottom: 1px solid %2; }").arg(chrome(colors).name(), wash(colors, 30));
    if (mainToolBar->styleSheet() != toolbarStyle) {
        mainToolBar->setStyleSheet(toolbarStyle);
    }
    const int iconSize = Thoughts::ReaderChrome::toolBarIconSize();
    if (mainToolBar->iconSize() != QSize(iconSize, iconSize)) {
        mainToolBar->setIconDimensions(iconSize);
    }
    if (Thoughts::ReaderChrome::mergesTitleBar()) {
        mainToolBar->setFixedHeight(Thoughts::ReaderChrome::toolBarHeight());
    }
    // Room for the window buttons, which go away in full screen.
    if (QAction *space = actionCollection()->action(QStringLiteral("window_buttons_space"))) {
        space->setVisible(Thoughts::ReaderChrome::mergesTitleBar() && !isFullScreen());
    }
    const QString hover = wash(colors, 13);
    const QString chosen = wash(colors, 26);
    for (QAction *action : mainToolBar->actions()) {
        auto *button = qobject_cast<QToolButton *>(mainToolBar->widgetForAction(action));
        if (!button) {
            continue;
        }
        const QString name = action->objectName();
        if (name == QLatin1String("view_render_mode") || name == QLatin1String("view_highlights")) {
            button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            button->setText(name == QLatin1String("view_highlights") ? i18n("Visible markup") : i18n("View"));
        } else if (name != QLatin1String("pdf_library_status")) {
            // Icons alone, with the name in the tooltip; Sync keeps its text, which is its status.
            button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        }
        if (name == QLatin1String("pdf_library_status")) {
            button->setAutoRaise(true);
        }
        const bool selected = name == QLatin1String("comments_overview") ? s_commentsOverview && s_commentsOverview->isActiveWindow()
            : name == QLatin1String("agent_notes")                       ? s_agentNotesWindow && s_agentNotesWindow->isActiveWindow()
                                                                         : name == QLatin1String("review_overview") && m_reviewOverview && m_reviewOverview->isActiveWindow();
        const QString buttonStyle = QStringLiteral(
            "QToolButton { border: 0; border-radius: 8px; padding: 5px 8px; background: %1; }"
            "QToolButton:hover { background: %2; }"
            "QToolButton:pressed, QToolButton:checked { background: %3; }"
            "QToolButton::menu-indicator { image: none; }").arg(selected ? chosen : QStringLiteral("transparent"), hover, chosen);
        if (button->styleSheet() != buttonStyle) {
            button->setStyleSheet(buttonStyle);
        }
    }
    updateDocumentTitle();
    syncPdfLibraryStatusToolButton();
}

void Shell::styleDocumentTabs()
{
    if (!m_tabWidget) {
        return;
    }
    using namespace Thoughts::ReaderTheme;
    const QPalette colors = m_tabWidget->palette();
    // Part of the bar above: the same surface, and the chosen tab only a soft wash.
    const QString style = QStringLiteral(
        "QTabBar { background: %1; border-bottom: 1px solid %2; }"
        "QTabBar::tab { border: 0; border-radius: 6px; padding: 3px 8px; margin: 3px 1px 4px 1px; background: transparent; color: %3; }"
        "QTabBar::tab:selected { background: %4; color: palette(text); }"
        "QTabBar::tab:hover:!selected { background: %5; }").arg(chrome(colors).name(), wash(colors, 30), wash(colors, 150), wash(colors, 20), wash(colors, 10));
    if (m_tabWidget->tabBar()->styleSheet() != style) {
        m_tabWidget->tabBar()->setStyleSheet(style);
    }
}

void Shell::updateDocumentTitle()
{
    if (!m_documentTitle) {
        return;
    }
    constexpr int maximumWidth = 280;
    // Without Qt's placeholder for the modified mark.
    const QString title = windowTitle().remove(QLatin1String("[*]")).trimmed();
    const QString shown = m_documentTitle->fontMetrics().elidedText(title, Qt::ElideRight, maximumWidth);
    if (m_documentTitle->text() != shown) {
        m_documentTitle->setText(shown);
        m_documentTitle->setToolTip(title);
        scheduleMainToolBarCentering();
    }
}

void Shell::applyReaderIcons(KActionCollection *actions)
{
    static constexpr std::pair<const char *, const char *> symbols[] = {
        {"sidebar_display", "sidebar.left"},
        // A highlighter, not an I-beam: the I-beam looked like the Text tool's box, and selecting
        // text is how a passage is liked, disliked or commented on.
        {"mouse_textselect", "highlighter"},
        {"mouse_select", "rectangle.dashed"},
        {"mouse_draw", "pencil"},
        {"mouse_text_box", "textbox"},
        {"go_document_back", "chevron.left"},
        {"go_document_forward", "chevron.right"},
        {"comments_overview", "list.bullet.rectangle"},
        {"review_overview", "rectangle.stack"},
    };
    const QColor tint = palette().color(QPalette::ButtonText);
    for (const auto &[name, symbol] : symbols) {
        if (QAction *action = actions->action(QLatin1StringView(name))) {
            // The pencil's button while it erases (the part says so in its data).
            const bool eraser = qstrcmp(name, "mouse_draw") == 0 && action->data().toString() == QLatin1String("eraser");
            action->setIcon(Thoughts::ReaderChrome::symbolIcon(eraser ? QStringLiteral("eraser") : QString(QLatin1StringView(symbol)), tint, action->icon()));
        }
    }
}

bool Shell::showPencilMenu(const QPoint &global)
{
    KToolBar *mainToolBar = toolBar(QStringLiteral("mainToolBar"));
    const int tab = m_tabWidget ? m_tabWidget->currentIndex() : -1;
    if (!mainToolBar || tab < 0 || tab >= m_tabs.size()) {
        return false;
    }
    QWidget *button = nullptr;
    for (QAction *action : mainToolBar->actions()) {
        if (action->objectName() == QLatin1String("mouse_draw")) {
            button = mainToolBar->widgetForAction(action);
            break;
        }
    }
    if (!button || !button->isVisible() || !button->rect().contains(button->mapFromGlobal(global))) {
        return false;
    }
    // The active tab's (the part's pencil has it): choosing there turns its pencil on, and every tab
    // follows the choice.
    QAction *pencil = m_tabs[tab].part->actionCollection()->action(QStringLiteral("mouse_draw"));
    auto *menu = pencil ? qobject_cast<QMenu *>(pencil->property("thoughtsMenu").value<QObject *>()) : nullptr;
    if (!menu) {
        return false;
    }
    menu->exec(global);
    // The pencil's or the eraser's icon, in every tab.
    applyReaderIcons(actionCollection());
    for (const TabState &state : std::as_const(m_tabs)) {
        applyReaderIcons(state.part->actionCollection());
    }
    return true;
}

void Shell::syncPdfLibraryStatusToolButton()
{
    KToolBar *mainToolBar = toolBar(QStringLiteral("mainToolBar"));
    if (!mainToolBar || !m_pdfLibraryWidgetAction || !mainToolBar->actions().contains(m_pdfLibraryWidgetAction)) {
        m_pdfBuildStatusButton = nullptr;
        return;
    }

    auto *button = qobject_cast<QToolButton *>(mainToolBar->widgetForAction(m_pdfLibraryWidgetAction));
    m_pdfBuildStatusButton = button;
    if (!button) {
        return;
    }

    button->setObjectName(QStringLiteral("pdfBuildStatusButton"));
    button->setAutoRaise(true);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    // One click starts Sync, with no menu or dropdown arrow.
    button->setMenu(nullptr);
    button->setText(m_pdfBuildStatusText);
    button->setAccessibleName(m_pdfBuildStatusAccessibleName);
    button->ensurePolished();

    QStyleOptionToolButton option;
    option.initFrom(button);
    option.toolButtonStyle = Qt::ToolButtonTextBesideIcon;
    option.iconSize = button->iconSize();
    option.font = button->font();
    const int textWidth = button->fontMetrics().horizontalAdvance(i18n("Sync"));
    const QSize contentSize(textWidth + option.iconSize.width() + 4, qMax(button->fontMetrics().height(), option.iconSize.height()));
    const int width = button->style()->sizeFromContents(QStyle::CT_ToolButton, &option, contentSize, button).width();
    if (button->minimumWidth() != width || button->maximumWidth() != width) {
        button->setFixedWidth(width);
        scheduleMainToolBarCentering();
    }
}

void Shell::showPdfSyncMenu()
{
    if (!m_pdfLibraryMenu) {
        return;
    }
    updatePdfLibraryAction();
    QWidget *anchor = m_pdfBuildStatusButton ? static_cast<QWidget *>(m_pdfBuildStatusButton.data()) : toolBar(QStringLiteral("mainToolBar"));
    if (!anchor) {
        anchor = this;
    }
    const int menuWidth = m_pdfLibraryMenu->sizeHint().width();
    m_pdfLibraryMenu->popup(anchor->mapToGlobal(QPoint(anchor->width() - menuWidth, anchor->height())));
}

void Shell::schedulePdfSyncStatusUpdate()
{
    // Builds stream thousands of events; refresh the status once per event-loop pass.
    if (m_pdfSyncStatusUpdateScheduled) {
        return;
    }
    m_pdfSyncStatusUpdateScheduled = true;
    QTimer::singleShot(0, this, [this] {
        m_pdfSyncStatusUpdateScheduled = false;
        updatePdfSyncStatus();
    });
}

void Shell::updatePdfSyncStatus()
{
    if (!m_pdfLibraryWidgetAction || !m_pdfSyncSummaryMenuAction || !m_pdfSyncDetailMenuAction || !m_pdfSyncNowMenuAction) {
        return;
    }
    if (m_globalPendingChangesSnapshot.isEmpty()) {
        QVariantMap pendingChanges;
        if (KParts::ReadWritePart *part = fedoraGatewayPart()) {
            QMetaObject::invokeMethod(part, "globalPendingChanges", Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, pendingChanges));
        }
        m_globalPendingChangesSnapshot = pendingChanges.isEmpty()
            ? QJsonObject {{QStringLiteral("available"), false}, {QStringLiteral("total"), 0}}
            : QJsonObject::fromVariantMap(pendingChanges);
    }

    const QJsonObject operation = latestGlobalSyncOperationSummary();
    const Thoughts::SyncOperationState lifecycle = Thoughts::syncOperationState(operation);
    const bool running = lifecycle == Thoughts::SyncOperationState::Running;
    const bool failed = lifecycle == Thoughts::SyncOperationState::Failed || lifecycle == Thoughts::SyncOperationState::Interrupted || lifecycle == Thoughts::SyncOperationState::Cancelled;
    const bool pending = m_globalPendingChangesSnapshot.value(QStringLiteral("total")).toInt() > 0;
    const bool known = m_globalPendingChangesSnapshot.value(QStringLiteral("available")).toBool()
        && m_globalPendingChangesSnapshot.value(QStringLiteral("source_state")).toString() != QLatin1String("unknown");
    const QString label = i18n("Sync");
    const QString iconName = QStringLiteral("view-refresh");
    QString summary = i18n("Synchronization");
    if (running) {
        const bool building = operation.value(QStringLiteral("kind")).toString() == QLatin1String("build");
        summary = building ? i18n("Building PDF") : Thoughts::SyncStatusLabels::syncing();
    } else if (failed) {
        summary = lifecycle == Thoughts::SyncOperationState::Failed ? Thoughts::syncOperationDisplayStatus(operation) : Thoughts::SyncStatusLabels::syncFailed();
    } else if (pending) {
        summary = Thoughts::SyncStatusLabels::changesPending();
    } else if (known && lifecycle == Thoughts::SyncOperationState::Succeeded) {
        summary = Thoughts::SyncStatusLabels::upToDate();
    }

    QString detail;
    const qint64 timestamp = operation.value(running ? QStringLiteral("created_at_ms") : QStringLiteral("updated_at_ms")).toVariant().toLongLong();
    if (failed) {
        detail = compactMenuDetail(Thoughts::syncOperationFailureMessage(operation));
    } else if (timestamp > 0) {
        // Locale formatting is costly and the timestamp rarely changes between refreshes.
        static qint64 formattedTimestamp = -1;
        static QString formatted;
        if (formattedTimestamp != timestamp) {
            formatted = QLocale().toString(QDateTime::fromMSecsSinceEpoch(timestamp).toLocalTime(), QLocale::ShortFormat);
            formattedTimestamp = timestamp;
        }
        const QString recordedAt = formatted;
        detail = running ? i18n("Started: %1", recordedAt) : i18n("Last sync: %1", recordedAt);
    } else {
        detail = i18n("No previous sync");
    }
    if (!running) {
        // "It keeps starting by itself" is said out loud: when the next automatic Sync may start.
        const QString notice = automaticSyncNotice();
        if (!notice.isEmpty()) {
            detail = detail.isEmpty() ? notice : QStringLiteral("%1 · %2").arg(detail, notice);
        }
    }
    m_pdfSyncSummaryMenuAction->setText(summary);
    m_pdfSyncDetailMenuAction->setText(detail);
    m_pdfSyncDetailMenuAction->setVisible(!detail.isEmpty());
    m_pdfSyncNowMenuAction->setText(failed ? i18n("Try again") : i18n("Sync now"));
    m_pdfSyncNowMenuAction->setEnabled(!running);
    m_pdfBuildStatusText = label;
    m_pdfBuildStatusAccessibleName = i18n("Sync with the server. %1", summary);
    m_pdfLibraryWidgetAction->setToolTip(QStringLiteral("%1 (%2)").arg(m_pdfBuildStatusAccessibleName, m_pdfLibraryWidgetAction->shortcut().toString(QKeySequence::NativeText)));
    if (m_pdfSyncIconName != iconName) {
        m_pdfSyncIconName = iconName;
        const QIcon themed = QIcon::fromTheme(iconName);
        m_pdfLibraryWidgetAction->setIcon(iconName == QLatin1String("view-refresh")
                                              ? Thoughts::ReaderChrome::symbolIcon(QStringLiteral("arrow.triangle.2.circlepath"), palette().color(QPalette::ButtonText), themed)
                                              : themed);
    }
    // Details remain available in File while duplicate network requests are disabled.
    m_pdfLibraryWidgetAction->setEnabled(!running && m_fedoraManualSyncState != 1 && !hasProcessManualSyncOperation() && m_activeBuildOperationId.isEmpty());
    syncPdfLibraryStatusToolButton();
}

void Shell::scheduleMainToolBarCentering()
{
    if (m_mainToolBarCenteringScheduled) {
        return;
    }
    m_mainToolBarCenteringScheduled = true;
    QTimer::singleShot(0, this, &Shell::centerMainToolBarPageSelector);
}

void Shell::centerMainToolBarPageSelector()
{
    m_mainToolBarCenteringScheduled = false;
    KToolBar *mainToolBar = toolBar(QStringLiteral("mainToolBar"));
    const int activeTab = m_tabWidget ? m_tabWidget->currentIndex() : -1;
    QAction *pageNumberAction = activeTab >= 0 && activeTab < m_tabs.size() ? m_tabs[activeTab].part->actionCollection()->action(QStringLiteral("page_number")) : nullptr;
    if (!mainToolBar || !pageNumberAction || !mainToolBar->actions().contains(pageNumberAction) || mainToolBar->orientation() != Qt::Horizontal) {
        return;
    }

    const QList<QAction *> actions = mainToolBar->actions();
    const int pageNumberIndex = actions.indexOf(pageNumberAction);
    QWidget *leftSpacer = nullptr;
    QWidget *rightSpacer = nullptr;
    for (int i = pageNumberIndex - 1; i >= 0 && !leftSpacer; --i) {
        QWidget *widget = mainToolBar->widgetForAction(actions.at(i));
        if (widget && widget->sizePolicy().expandingDirections().testFlag(Qt::Horizontal)) {
            leftSpacer = widget;
        }
    }
    for (int i = pageNumberIndex + 1; i < actions.size() && !rightSpacer; ++i) {
        QWidget *widget = mainToolBar->widgetForAction(actions.at(i));
        if (widget && widget->sizePolicy().expandingDirections().testFlag(Qt::Horizontal)) {
            rightSpacer = widget;
        }
    }
    QWidget *pageNumberWidget = mainToolBar->widgetForAction(pageNumberAction);
    if (!leftSpacer || !rightSpacer || !pageNumberWidget) {
        return;
    }

    // Measure every configured action, including ones in overflow. Never reset
    // spacers to expanding widths between frames: that made the toolbar oscillate.
    const auto preferredWidth = [](QWidget *widget) {
        return widget->sizeHint().expandedTo(widget->minimumSizeHint()).expandedTo(widget->minimumSize()).boundedTo(widget->maximumSize()).width();
    };
    const QMargins margins = mainToolBar->layout()->contentsMargins();
    const int spacing = qMax(0, mainToolBar->layout()->spacing());
    int leading = margins.left();
    int trailing = margins.right();
    if (mainToolBar->isMovable()) {
        leading += mainToolBar->style()->pixelMetric(QStyle::PM_ToolBarHandleExtent, nullptr, mainToolBar);
    }
    for (int i = 0; i < actions.size(); ++i) {
        if (i == pageNumberIndex || !actions.at(i)->isVisible()) {
            continue;
        }
        QWidget *widget = mainToolBar->widgetForAction(actions.at(i));
        if (!widget) {
            continue;
        }
        const int width = widget == leftSpacer || widget == rightSpacer ? 0 : qMax(0, preferredWidth(widget));
        (i < pageNumberIndex ? leading : trailing) += width + spacing;
    }
    const int pageWidth = preferredWidth(pageNumberWidget);
    const int available = qMax(0, mainToolBar->contentsRect().width() - leading - trailing - pageWidth);
    const int centeredLeft = (mainToolBar->contentsRect().width() - pageWidth) / 2;
    const int leftWidth = qBound(0, centeredLeft - leading, available);
    const int rightWidth = available - leftWidth;
    if (leftSpacer->minimumWidth() != leftWidth || leftSpacer->maximumWidth() != leftWidth) {
        leftSpacer->setFixedWidth(leftWidth);
    }
    if (rightSpacer->minimumWidth() != rightWidth || rightSpacer->maximumWidth() != rightWidth) {
        rightSpacer->setFixedWidth(rightWidth);
    }
}

void Shell::closeTab(int tab)
{
    closeTabInternal(tab, true);
}

bool Shell::closeTabInternal(int tab, bool promptToSave)
{
    if (tab < 0 || tab >= m_tabs.size()) {
        return false;
    }

    KParts::ReadWritePart *const part = m_tabs[tab].part;
    QUrl url = part->url();
    const QString page = part->property(HTML_ATTACHMENT_TAB_PROPERTY).toString();
    const bool closeSuccess = part->closeUrl(promptToSave);
    if (closeSuccess) {
        forgetNavigationHistory(part);
    }
    if (closeSuccess && m_tabs.count() > 1) {
        if (part->factory()) {
            part->factory()->removeClient(part);
        }
        // Closing a page's tab shows its document's tab again (not the tab beside it).
        if (!page.isEmpty() && tab == m_tabWidget->currentIndex()) {
            const int document = findTabIndex(url);
            if (document >= 0) {
                setActiveTab(document);
                tab = findTabIndex(part);
            }
        }
        part->disconnect(this);

        Okular::ViewerInterface *iPart = qobject_cast<Okular::ViewerInterface *>(m_tabs[tab].part);
        Q_ASSERT(iPart);
        QWidget *sideContainer = iPart->getSideContainer();
        m_sidebar->removeWidget(sideContainer);
        connect(part, &QObject::destroyed, sideContainer, &QObject::deleteLater);

        part->deleteLater();
        m_tabs.removeAt(tab);
        m_tabWidget->removeTab(tab);
        m_undoCloseTab->setEnabled(true);
        m_closedTabUrls.append(url);
        m_closedTabPages.append(page);
        updateTabControls();
    } else if (closeSuccess && m_tabs.count() == 1) {
        // The window's last tab keeps its Part for the next document: an ordinary one again.
        if (!page.isEmpty()) {
            part->setProperty(HTML_ATTACHMENT_TAB_PROPERTY, QVariant());
            QMetaObject::invokeMethod(part, "leaveHtmlAttachmentTab", Qt::DirectConnection);
        }
        showOverviewHome();
        updatePdfLibraryAction();
    }
    return closeSuccess;
}

void Shell::recordNavigationViewport(bool historyEntry)
{
    if (m_tabWidget->currentIndex() >= 0 && sender() == m_tabs[m_tabWidget->currentIndex()].part) {
        recordCurrentNavigationViewport(historyEntry);
    }
}

void Shell::recordCurrentNavigationViewport(bool historyEntry)
{
    // A page's tab has no place in its PDF to go back to.
    if (m_restoringNavigation || m_tabWidget->currentIndex() < 0 || isHtmlAttachmentTab(m_tabWidget->currentIndex())) {
        return;
    }
    auto *part = m_tabs[m_tabWidget->currentIndex()].part;
    QString viewport;
    QMetaObject::invokeMethod(part, "navigationViewport", Q_RETURN_ARG(QString, viewport));
    if (part->url().isEmpty() || viewport.isEmpty()) {
        return;
    }
    const NavigationLocation location {part, part->url(), viewport};
    if (m_navigationHistoryIndex >= 0) {
        auto &current = m_navigationHistory[m_navigationHistoryIndex];
        if (current.part == part && current.url == location.url && (!historyEntry || current.viewport == viewport)) {
            current.viewport = viewport;
            updateNavigationHistoryActions();
            return;
        }
    }
    // Following a new link after Back starts a new branch of the history.
    m_navigationHistory.resize(m_navigationHistoryIndex + 1);
    m_navigationHistory.append(location);
    if (m_navigationHistory.size() > 100) {
        m_navigationHistory.removeFirst();
    }
    m_navigationHistoryIndex = m_navigationHistory.size() - 1;
    updateNavigationHistoryActions();
}

void Shell::updateNavigationHistoryActions()
{
    for (const auto &tab : std::as_const(m_tabs)) {
        QMetaObject::invokeMethod(tab.part, "setShellNavigationHistory",
                                 Q_ARG(bool, m_navigationHistoryIndex > 0),
                                 Q_ARG(bool, m_navigationHistoryIndex >= 0 && m_navigationHistoryIndex + 1 < m_navigationHistory.size()));
    }
}

void Shell::forgetNavigationHistory(KParts::ReadWritePart *part)
{
    for (int index = m_navigationHistory.size() - 1; index >= 0; --index) {
        if (!m_navigationHistory[index].part || m_navigationHistory[index].part == part) {
            m_navigationHistory.removeAt(index);
            if (index <= m_navigationHistoryIndex) {
                --m_navigationHistoryIndex;
            }
        }
    }
    updateNavigationHistoryActions();
}

void Shell::navigateHistory(bool forward)
{
    const int step = forward ? 1 : -1;
    for (int index = m_navigationHistoryIndex + step; index >= 0 && index < m_navigationHistory.size(); index += step) {
        const NavigationLocation location = m_navigationHistory[index];
        const int tab = findTabIndex(location.part);
        if (!location.part || tab < 0 || location.part->url() != location.url) {
            continue;
        }
        bool restored = false;
        {
            QScopedValueRollback<bool> guard(m_restoringNavigation, true);
            setActiveTab(tab);
            QMetaObject::invokeMethod(location.part, "navigateToDestination", Q_RETURN_ARG(bool, restored),
                                      Q_ARG(QString, QString()), Q_ARG(QString, location.viewport), Q_ARG(bool, false));
        }
        if (restored) {
            m_navigationHistoryIndex = index;
            updateNavigationHistoryActions();
            return;
        }
    }
}

QUrl Shell::documentLinkTarget(const QUrl &url, const QString &docsRoot)
{
    if (!url.isLocalFile() || QFileInfo::exists(url.toLocalFile()) || url.fileName().isEmpty()) {
        return url;
    }
    // Links to another Library PDF can keep the path they had in the build
    // folder (../../.thoughts/projects/<id>/default/<file>.pdf), which means
    // nothing next to current_pdfs. current_pdfs holds every published PDF
    // under its file name, as the phone resolves these links.
    const QString published = QDir(docsRoot).filePath(QStringLiteral("current_pdfs/") + url.fileName());
    return QFileInfo::exists(published) ? QUrl::fromLocalFile(published) : url;
}

void Shell::openDocumentLink(const QUrl &linkUrl, const QString &destination, const QString &viewport)
{
    const int sourceIndex = findTabIndex(sender());
    if (sourceIndex < 0 || sourceIndex != m_tabWidget->currentIndex()) {
        return;
    }
    const QUrl url = documentLinkTarget(linkUrl, ShellUtils::docsRoot());
    if (url.isLocalFile() && !QFileInfo::exists(url.toLocalFile())) {
        KMessageBox::error(this, i18n("Could not open '%1'. File does not exist", url.toDisplayString()));
        return;
    }
    recordCurrentNavigationViewport(false);
    bool opened = false;
    {
        // Do not record the destination PDF's initial page before its link
        // destination has been resolved, or Back would land on that page.
        QScopedValueRollback<bool> guard(m_restoringNavigation, true);
        const int existing = findTabIndex(url);
        if (existing >= 0) {
            setActiveTab(existing);
        } else {
            openNewTab(url, QString());
        }
        auto *target = m_tabs[m_tabWidget->currentIndex()].part;
        opened = findTabIndex(url) == m_tabWidget->currentIndex();
        if (opened && (!destination.isEmpty() || !viewport.isEmpty())) {
            bool located = false;
            QMetaObject::invokeMethod(target, "navigateToDestination", Q_RETURN_ARG(bool, located),
                                      Q_ARG(QString, destination), Q_ARG(QString, viewport), Q_ARG(bool, true));
            if (!located) {
                KMessageBox::information(this, i18n("The document is open, but the linked location could not be found in this version."));
            }
        }
    }
    if (opened) {
        recordCurrentNavigationViewport(true);
    }
}

bool Shell::isHtmlAttachmentTab(int tab) const
{
    return tab >= 0 && tab < m_tabs.size() && m_tabs[tab].part && !m_tabs[tab].part->property(HTML_ATTACHMENT_TAB_PROPERTY).toString().isEmpty();
}

int Shell::findHtmlAttachmentTab(const QUrl &pdf, const QString &path) const
{
    const QUrl target = pdf.adjusted(QUrl::RemoveFragment | QUrl::NormalizePathSegments);
    for (int index = 0; index < m_tabs.size(); ++index) {
        if (m_tabs[index].part->property(HTML_ATTACHMENT_TAB_PROPERTY).toString() == path
            && m_tabs[index].part->url().adjusted(QUrl::RemoveFragment | QUrl::NormalizePathSegments) == target) {
            return index;
        }
    }
    return -1;
}

void Shell::openHtmlAttachmentTab(const QJsonObject &request)
{
    const int source = findTabIndex(sender());
    if (source < 0) {
        return;
    }
    // After the pages already opened from this document, as a browser opens links in new tabs.
    int beside = source;
    while (beside + 1 < m_tabs.size() && isHtmlAttachmentTab(beside + 1) && m_tabs[beside + 1].part->url() == m_tabs[source].part->url()) {
        ++beside;
    }
    KParts::ReadWritePart *const sourcePart = m_tabs[source].part;
    KParts::ReadWritePart *const part = showHtmlAttachmentTab(sourcePart->url(), request, beside);
    // The document's chosen title names the page's document too.
    if (part && part != sourcePart) {
        for (const char *name : {"thoughtsTitleDocumentId", "thoughtsTitleProjectId", "thoughtsTitleFallback", "thoughtsTitleUrl", "thoughtsTitlePdfId"}) {
            if (!part->property(name).isValid()) part->setProperty(name, sourcePart->property(name));
        }
        refreshReaderTabTitles();
    }
}

KParts::ReadWritePart *Shell::showHtmlAttachmentTab(const QUrl &pdf, const QJsonObject &request, int besideTab)
{
    const QString path = request.value(QStringLiteral("path")).toString();
    if (path.isEmpty() || !pdf.isLocalFile()) {
        return nullptr;
    }
    const int existing = findHtmlAttachmentTab(pdf, path);
    if (existing >= 0) {
        KParts::ReadWritePart *const part = m_tabs[existing].part;
        setActiveTab(existing);
        bool shown = false;
        QMetaObject::invokeMethod(part, "showInHtmlAttachmentTab", Qt::DirectConnection, Q_RETURN_ARG(bool, shown), Q_ARG(QJsonObject, request));
        return part;
    }
    const QPointer<QWidget> previous = m_tabWidget->currentWidget();
    // The window's empty tab (a restored session, Undo Close Tab) gives way, as for a moved tab.
    removeEmptyPlaceholderPart();
    // A second Part of the same document: the page keeps its owner (cards, feedback, Sync) and the
    // document stays where it was in its own tab. It goes right after @p besideTab.
    KParts::ReadWritePart *const part = m_partFactory->create<KParts::ReadWritePart>(this);
    part->setProperty(HTML_ATTACHMENT_TAB_PROPERTY, path);
    const int index = besideTab >= 0 ? qBound(0, besideTab + 1, int(m_tabs.size())) : int(m_tabs.size());
    {
        const QSignalBlocker blocker(m_tabWidget);
        m_tabs.insert(index, TabState(part));
        m_tabWidget->insertTab(index, part->widget(), QFileInfo(path).completeBaseName());
    }
    connectPart(part);
    m_centralStackedWidget->setCurrentWidget(m_tabWidget);
    updateTabControls();
    setActiveTab(index);
    hideWelcomeScreen();
    bool shown = false;
    QMetaObject::invokeMethod(part, "openHtmlAttachmentTab", Qt::DirectConnection, Q_RETURN_ARG(bool, shown), Q_ARG(QUrl, pdf), Q_ARG(QJsonObject, request));
    if (!shown) {
        if (previous && m_tabWidget->indexOf(previous) >= 0) {
            setActiveTab(m_tabWidget->indexOf(previous));
        }
        // A page that never showed is nothing to reopen.
        const qsizetype closed = m_closedTabUrls.size();
        closeTabInternal(findTabIndex(part), false);
        while (m_closedTabUrls.size() > closed) {
            m_closedTabUrls.removeLast();
            m_closedTabPages.removeLast();
        }
        m_undoCloseTab->setEnabled(!m_closedTabUrls.isEmpty());
        return nullptr;
    }
    refreshReaderTabTitles();
    return part;
}

void Shell::closeHtmlAttachmentTab()
{
    const int tab = findTabIndex(sender());
    if (isHtmlAttachmentTab(tab)) {
        closeTab(tab);
    }
}

void Shell::openNewTab(const QUrl &url, const QString &serializedOptions)
{
    const int previousActiveTab = m_tabWidget->currentIndex();
    KParts::ReadWritePart *const activePart = m_tabs[previousActiveTab].part;

    hideWelcomeScreen();

    bool activateTabIfAlreadyOpen;
    QMetaObject::invokeMethod(activePart, "activateTabIfAlreadyOpenFile", Q_RETURN_ARG(bool, activateTabIfAlreadyOpen));

    if (activateTabIfAlreadyOpen) {
        const int tabIndex = findTabIndex(url);

        if (tabIndex >= 0) {
            setActiveTab(tabIndex);
            m_recent->addUrl(url);
            return;
        }
    }

    const int newIndex = m_tabs.size();

    // Make new part
    m_tabs.append(TabState(m_partFactory->create<KParts::ReadWritePart>(this)));
    connectPart(m_tabs[newIndex].part);

    // Update GUI
    KParts::ReadWritePart *const part = m_tabs[newIndex].part;
    m_tabWidget->addTab(part->widget(), url.fileName());
    m_tabWidget->setTabToolTip(newIndex, url.fileName());
    updateTabControls();

    applyOptionsToPart(part, serializedOptions);

    setActiveTab(m_tabs.size() - 1);

    if (part->openUrl(url)) {
        m_recent->addUrl(url);
        updatePdfLibraryAction();
    } else {
        setActiveTab(previousActiveTab);
        closeTab(m_tabs.size() - 1);
        m_recent->removeUrl(url);
    }
}

void Shell::applyOptionsToPart(QObject *part, const QString &serializedOptions)
{
    KDocumentViewer *const doc = qobject_cast<KDocumentViewer *>(part);
    const QString find = ShellUtils::find(serializedOptions);
    if (ShellUtils::startInPresentation(serializedOptions)) {
        doc->startPresentation();
    }
    if (ShellUtils::showPrintDialog(serializedOptions)) {
        QMetaObject::invokeMethod(part, "enableStartWithPrint");
    }
    if (ShellUtils::showPrintDialogAndExit(serializedOptions)) {
        QMetaObject::invokeMethod(part, "enableExitAfterPrint");
    }
    if (!find.isEmpty()) {
        QMetaObject::invokeMethod(part, "enableStartWithFind", Q_ARG(QString, find));
    }
}

void Shell::connectPart(const KParts::ReadWritePart *part)
{
    attachAgentNotesSidebar(const_cast<KParts::ReadWritePart *>(part));
    // We're abusing the fact we know the part is our part here
    connect(this, SIGNAL(moveSplitter(int)), part, SLOT(moveSplitter(int)));                                                                                                                               // clazy:exclude=old-style-connect
    connect(part, SIGNAL(enablePrintAction(bool)), this, SLOT(setPrintEnabled(bool)));                                                                                                                     // clazy:exclude=old-style-connect
    connect(part, SIGNAL(enableCloseAction(bool)), this, SLOT(setCloseEnabled(bool)));                                                                                                                     // clazy:exclude=old-style-connect
    applyReaderIcons(part->actionCollection());
    connect(part, SIGNAL(mimeTypeChanged(QMimeType)), this, SLOT(setTabIcon(QMimeType)));                                                                                                                  // clazy:exclude=old-style-connect
    connect(part, SIGNAL(urlsDropped(QList<QUrl>)), this, SLOT(handleDroppedUrls(QList<QUrl>)));                                                                                                           // clazy:exclude=old-style-connect
    connect(part, SIGNAL(maxRecentItemsChanged(int)), this, SLOT(triggerUpdateRecentItems(int)));                                                                                                          // clazy:exclude=old-style-connect
    connect(part, SIGNAL(requestOpenDocumentLink(QUrl, QString, QString)), this, SLOT(openDocumentLink(QUrl, QString, QString))); // clazy:exclude=old-style-connect
    connect(part, SIGNAL(requestHtmlAttachmentTab(QJsonObject)), this, SLOT(openHtmlAttachmentTab(QJsonObject))); // clazy:exclude=old-style-connect
    connect(part, SIGNAL(requestCloseHtmlAttachmentTab()), this, SLOT(closeHtmlAttachmentTab())); // clazy:exclude=old-style-connect
    connect(part, SIGNAL(requestOpenDocumentFeedback(QString, QString)), this, SLOT(openTrackedFeedback(QString, QString))); // clazy:exclude=old-style-connect
    connect(part, SIGNAL(navigationViewportChanged(bool)), this, SLOT(recordNavigationViewport(bool))); // clazy:exclude=old-style-connect
    connect(part, SIGNAL(requestNavigationHistory(bool)), this, SLOT(navigateHistory(bool))); // clazy:exclude=old-style-connect
    connect(part, &KParts::ReadOnlyPart::completed, this, [this, part] {
        if (m_tabWidget->currentIndex() >= 0 && m_tabs[m_tabWidget->currentIndex()].part == part) {
            recordCurrentNavigationViewport(false);
        }
    });
    connect(part, SIGNAL(requestCommentsOverview()), this, SLOT(showCommentsOverview()));                                                                                                                  // clazy:exclude=old-style-connect
    connect(part, SIGNAL(requestSyncOperations()), this, SLOT(showSyncOperations()));                                                                                                                       // clazy:exclude=old-style-connect
    connect(part, SIGNAL(requestOpenCommentCodex(QString, QString)), this, SLOT(openCommentCodex(QString, QString)));                                                                                      // clazy:exclude=old-style-connect
    connect(part, SIGNAL(requestOpenCommentChatGpt(QString)), this, SLOT(openCommentChatGpt(QString)));                                                                                                    // clazy:exclude=old-style-connect
    connect(part, SIGNAL(requestOpenSourceSelection(QString, int, QString)), this, SLOT(openSourceSelection(QString, int, QString))); // clazy:exclude=old-style-connect
    connect(part, SIGNAL(requestOpenCommentSource(QString, int)), this, SLOT(openCommentSource(QString, int)));                                                                                            // clazy:exclude=old-style-connect
    connect(part, SIGNAL(libraryImportFinished(QString, QString, QString, QString, bool, int, QString)), this, SLOT(handleLibraryImportFinished(QString, QString, QString, QString, bool, int, QString))); // clazy:exclude=old-style-connect
    connect(part, SIGNAL(libraryDisplayNameFinished(QString, QString, QString, QString, bool, QString)), this, SLOT(handleLibraryDisplayNameFinished(QString, QString, QString, QString, bool, QString))); // clazy:exclude=old-style-connect
    connect(part, SIGNAL(libraryRemovalFinished(QString, QString, QString, bool, QString)), this, SLOT(handleLibraryRemovalFinished(QString, QString, QString, bool, QString)));                           // clazy:exclude=old-style-connect
    connect(part, SIGNAL(fedoraProjectionsChanged(QJsonObject, qint64)), this, SLOT(updateCommentsOverviewFedoraProjections(QJsonObject, qint64)));                                                        // clazy:exclude=old-style-connect
    connect(part, SIGNAL(pendingChangesChanged(QJsonObject)), this, SLOT(updateCommentsOverviewPendingChanges(QJsonObject)));                                                                              // clazy:exclude=old-style-connect
    connect(part, SIGNAL(fedoraManualSyncStateChanged(int, QString, QString)), this, SLOT(updateCommentsOverviewManualSyncState(int, QString, QString)));                                                  // clazy:exclude=old-style-connect
    connect(part, SIGNAL(fedoraRemoteWorkFound(bool, QString)), this, SLOT(remoteWorkAnnounced(bool, QString))); // clazy:exclude=old-style-connect
    connect(part, SIGNAL(syncProgressChanged(QString, QString, qint64, qint64, QString)), this, SLOT(updateSyncOperationProgress(QString, QString, qint64, qint64, QString)));                             // clazy:exclude=old-style-connect
    connect(part, SIGNAL(syncTransferProgressChanged(QString, QJsonObject)), this, SLOT(updateSyncOperationTransferProgress(QString, QJsonObject)));                                                       // clazy:exclude=old-style-connect
    connect(part, SIGNAL(syncJournalEvent(QString, QJsonObject)), this, SLOT(updateSyncOperationJournalEvent(QString, QJsonObject)));                                                                      // clazy:exclude=old-style-connect
    // A Sync started by a window that has since closed reports through this tab from now on.
    QTimer::singleShot(0, this, [this] { reconcileOrphanedManualSync(); });
    connect(part, SIGNAL(globalSyncRequested()), this, SLOT(triggerFedoraSync()));                                                                                                                         // clazy:exclude=old-style-connect
    connect(part, SIGNAL(fedoraRenderStatusChanged(QString, QJsonObject)), this, SLOT(handleFedoraRenderStatusChanged(QString, QJsonObject)));                                                             // clazy:exclude=old-style-connect
    connect(part, SIGNAL(fedoraRenderStateChanged(QJsonObject)), this, SLOT(updateCommentsOverviewRenderState(QJsonObject)));                                                                              // clazy:exclude=old-style-connect
    connect(part,
            SIGNAL(agentProposalResolutionStatusChanged(QString, QStringList, QString, QString, QString)),
            this,
            SLOT(updateCommentsOverviewAgentProposalResolution(QString, QStringList, QString, QString, QString))); // clazy:exclude=old-style-connect
    connect(part, &KParts::ReadOnlyPart::urlChanged, this, [this, part](const QUrl &) {
        schedulePdfLibraryPartRefresh(part);
        refreshReaderTabTitles();
    });
    connect(part, &KParts::ReadOnlyPart::completed, this, [this] { refreshReaderTabTitles(); });
    connect(part, &KParts::ReadOnlyPart::completed, this, [this, part] { schedulePdfLibraryPartRefresh(part); });
    connect(part, &KParts::ReadOnlyPart::completed, this, [this, part] {
        auto *mutablePart = const_cast<KParts::ReadWritePart *>(part);
        QJsonObject projections;
        QMetaObject::invokeMethod(mutablePart, "effectiveFedoraProjections", Qt::DirectConnection, Q_RETURN_ARG(QJsonObject, projections));
        refreshSidebarReviewCards(mutablePart, projections);
    });
    connect(part, &KParts::ReadOnlyPart::canceled, this, [this, part](const QString &) { schedulePdfLibraryPartRefresh(part); });
    connect(part, SIGNAL(requestManualReviewCard(QString, int, QVariantList, bool)), this, SLOT(createManualReviewCard(QString, int, QVariantList, bool))); // clazy:exclude=old-style-connect
    connect(part, SIGNAL(requestManualReviewCardAreas(QString, QVariantList, bool)), this, SLOT(createManualReviewCardAreas(QString, QVariantList, bool))); // clazy:exclude=old-style-connect
    connect(part, SIGNAL(requestHtmlReviewCard(QString, QVariantMap)), this, SLOT(createHtmlReviewCard(QString, QVariantMap))); // clazy:exclude=old-style-connect
    connect(part, SIGNAL(requestEditReviewCard(QString)), this, SLOT(editSidebarReviewCard(QString))); // clazy:exclude=old-style-connect
    connect(part, SIGNAL(requestOpenReviewCard(QString)), this, SLOT(openSidebarReviewCard(QString))); // clazy:exclude=old-style-connect
    connect(part, SIGNAL(requestReviewReviewCard(QString)), this, SLOT(reviewSidebarReviewCard(QString))); // clazy:exclude=old-style-connect
    connect(part, SIGNAL(requestMergeReviewCards(QStringList)), this, SLOT(mergeSidebarReviewCards(QStringList))); // clazy:exclude=old-style-connect
    connect(part, SIGNAL(requestOutlineDocumentActions(QMenu*, QString, int, QString)), this, SLOT(addOutlineDocumentActions(QMenu*, QString, int, QString))); // clazy:exclude=old-style-connect
    connect(part, SIGNAL(requestCommentsOverviewForComment(QString, QString, int)), this, SLOT(showCommentInOverview(QString, QString, int)));  // clazy:exclude=old-style-connect
    connect(part, SIGNAL(unplacedCommentOpened(QString, int)), this, SLOT(recordUnplacedCommentOpened(QString, int)));  // clazy:exclude=old-style-connect
    connect(part, SIGNAL(developerEvent(QString, QJsonObject)), this, SLOT(recordPartDeveloperEvent(QString, QJsonObject)));  // clazy:exclude=old-style-connect
    // clang-format off
    connect(part, SIGNAL(requestOpenNewlySignedFile(QString,int)), this, SLOT(openNewlySignedFile(QString,int))); // clazy:exclude=old-style-connect
    // Otherwise the QSize,QSize gets turned into QSize, QSize that is not normalized signals and is slightly slower
    connect(part, SIGNAL(fitWindowToPage(QSize,QSize)), this, SLOT(slotFitWindowToPage(QSize,QSize)));   // clazy:exclude=old-style-connect
    // clang-format on
}

void Shell::print()
{
    QMetaObject::invokeMethod(m_tabs[m_tabWidget->currentIndex()].part, "slotPrint");
}

void Shell::setPrintEnabled(bool enabled)
{
    int i = findTabIndex(sender());
    if (i != -1) {
        m_tabs[i].printEnabled = enabled;
        if (i == m_tabWidget->currentIndex()) {
            m_printAction->setEnabled(enabled);
        }
    }
}

void Shell::setCloseEnabled(bool enabled)
{
    int i = findTabIndex(sender());
    if (i != -1) {
        m_tabs[i].closeEnabled = enabled;
        if (i == m_tabWidget->currentIndex()) {
            m_closeAction->setEnabled(enabled);
        }
    }
}

void Shell::activateNextTab()
{
    if (m_tabs.size() < 2) {
        return;
    }

    const int activeTab = m_tabWidget->currentIndex();
    const int nextTab = (activeTab == m_tabs.size() - 1) ? 0 : activeTab + 1;

    setActiveTab(nextTab);
}

void Shell::activatePrevTab()
{
    if (m_tabs.size() < 2) {
        return;
    }

    const int activeTab = m_tabWidget->currentIndex();
    const int prevTab = (activeTab == 0) ? m_tabs.size() - 1 : activeTab - 1;

    setActiveTab(prevTab);
}

void Shell::undoCloseTab()
{
    if (m_closedTabUrls.isEmpty()) {
        return;
    }

    const QUrl lastTabUrl = m_closedTabUrls.takeLast();
    const QString page = m_closedTabPages.isEmpty() ? QString() : m_closedTabPages.takeLast();

    if (m_closedTabUrls.isEmpty()) {
        m_undoCloseTab->setEnabled(false);
    }

    // A page's tab again, next to its document's when that is open.
    if (!page.isEmpty()) {
        showHtmlAttachmentTab(lastTabUrl, {{QStringLiteral("path"), page}}, findTabIndex(lastTabUrl));
        return;
    }
    openUrl(lastTabUrl);
}

void Shell::setTabIcon(const QMimeType &mimeType)
{
    int i = findTabIndex(sender());
    if (i != -1) {
        // Tabs are pills with their title alone where the window has merged its title bar. A
        // page's tab shows a page, whatever its document is.
        const QString icon = isHtmlAttachmentTab(i) ? QStringLiteral("text-html") : mimeType.iconName();
        m_tabWidget->setTabIcon(i, Thoughts::ReaderChrome::mergesTitleBar() ? QIcon() : QIcon::fromTheme(icon));
    }
}

int Shell::findTabIndex(QObject *sender) const
{
    for (int i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs[i].part == sender) {
            return i;
        }
    }
    return -1;
}

int Shell::findTabIndex(const QUrl &url) const
{
    const QUrl target = url.adjusted(QUrl::RemoveFragment | QUrl::NormalizePathSegments);
    const QString canonicalPath = target.isLocalFile() ? QFileInfo(target.toLocalFile()).canonicalFilePath() : QString();
    auto it = std::find_if(m_tabs.begin(), m_tabs.end(), [&target, &canonicalPath](const TabState &state) {
        // A page's tab has its document open but is not the document's tab.
        if (!state.part->property(HTML_ATTACHMENT_TAB_PROPERTY).toString().isEmpty()) {
            return false;
        }
        const QUrl open = state.part->url().adjusted(QUrl::RemoveFragment | QUrl::NormalizePathSegments);
        return open == target || (!canonicalPath.isEmpty() && open.isLocalFile() && QFileInfo(open.toLocalFile()).canonicalFilePath() == canonicalPath);
    });
    return (it != m_tabs.end()) ? std::distance(m_tabs.begin(), it) : -1;
}

void Shell::handleDroppedUrls(const QList<QUrl> &urls)
{
    for (const QUrl &url : urls) {
        openUrl(url);
    }
}

void Shell::moveTabData(int from, int to)
{
    m_tabs.move(from, to);
}

void Shell::slotFitWindowToPage(const QSize pageViewSize, const QSize pageSize)
{
    const int xOffset = pageViewSize.width() - pageSize.width();
    const int yOffset = pageViewSize.height() - pageSize.height();
    showNormal();
    resize(width() - xOffset, height() - yOffset);
    Q_EMIT moveSplitter(pageSize.width());
}

void Shell::hideWelcomeScreen()
{
    m_centralStackedWidget->setCurrentWidget(m_tabWidget);
    updateTabControls();
    m_sidebar->setVisible(m_showSidebarAction->isChecked());
    m_showSidebarAction->setEnabled(true);
}

void Shell::showWelcomeScreen()
{
    m_showSidebarAction->setEnabled(false);
    m_centralStackedWidget->setCurrentWidget(m_welcomeScreen);
    m_tabWidget->tabBar()->hide();
    m_nextTabAction->setEnabled(false);
    m_prevTabAction->setEnabled(false);
    m_sidebar->setVisible(false);

    refreshRecentsOnWelcomeScreen();
}

bool Shell::hasOpenDocument() const
{
    return std::any_of(m_tabs.cbegin(), m_tabs.cend(), [](const TabState &tab) { return tab.part && !tab.part->url().isEmpty(); });
}

void Shell::showOverviewHome()
{
    showWelcomeScreen();
    showCommentsOverview(true);
    hide();
}

void Shell::refreshRecentsOnWelcomeScreen()
{
    saveRecents();
    m_welcomeScreen->loadRecents();
}

void Shell::forgetRecentItem(QUrl const &url)
{
    if (m_recent != nullptr) {
        m_recent->removeUrl(url);
        saveRecents();
        refreshRecentsOnWelcomeScreen();
    }
}

void Shell::triggerUpdateRecentItems(const int maxItems)
{
    m_recent->setMaxItems(maxItems);
    m_welcomeScreen->setMaxRecentItems(m_recent->maxItems());
    // saveRecents() dumps the recent files in correct order to KConfigGroup, respecting the allowed no. of recent items, discarding older items if needed
    refreshRecentsOnWelcomeScreen();
    m_recent->loadEntries(KSharedConfig::openConfig()->group(RecentFilesGroupKey()));
}

void Shell::readRecentFilesSettings()
{
    // Read the configured maximum number of recent items and populate both recent-file views.
    QString configFilePath = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + QLatin1Char('/') + QLatin1String("thoughtspartrc");
    const KConfigGroup confgrp = KSharedConfig::openConfig(configFilePath).data()->group(QStringLiteral("General"));
    const int defaultMaxRecentItems = 10;
    int maxRecentItems = confgrp.readEntry<int>("MaxRecentItems", defaultMaxRecentItems);
    m_recent->setMaxItems(maxRecentItems);
    m_welcomeScreen->setMaxRecentItems(m_recent->maxItems());
    m_welcomeScreen->loadRecents();
    m_recent->loadEntries(KSharedConfig::openConfig()->group(RecentFilesGroupKey()));
}

#include "shell.moc"

/* kate: replace-tabs on; indent-width 4; */

void Shell::showBuildSourceDiff(const QString &buildId, const QJsonObject &diff, const QString &error)
{
    Q_EMIT buildSourceDiffFinished(buildId);
    if (!error.isEmpty()) {
        KMessageBox::error(this, error, i18n("Show diff"));
        return;
    }
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("buildSourceDiffDialog"));
    dialog.setWindowTitle(i18n("Source changes for this build"));
    dialog.resize(850, 480);
    auto *layout = new QVBoxLayout(&dialog);
    auto *description = new QLabel(i18n("Compare the last successful PDF source (left) with your editable current source (right). Save changes in VS Code, then run Sync when ready. The failed-build snapshot remains available separately."), &dialog);
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *files = new QTreeWidget(&dialog);
    files->setObjectName(QStringLiteral("buildSourceDiffFiles"));
    files->setColumnCount(2);
    files->setHeaderLabels({i18n("Document / file"), i18n("Change")});
    files->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(files, 1);
    auto *diagnostic = new QLabel(&dialog);
    diagnostic->setTextFormat(Qt::PlainText);
    diagnostic->setWordWrap(true);
    diagnostic->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(diagnostic);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto *open = buttons->addButton(i18n("Open diff in VS Code"), QDialogButtonBox::ActionRole);
    auto *snapshot = buttons->addButton(i18n("View failed-build snapshot"), QDialogButtonBox::ActionRole);
    snapshot->setObjectName(QStringLiteral("viewFailedBuildSnapshot"));
    layout->addWidget(buttons);
    QTreeWidgetItem *initial = nullptr;
    for (const auto &value : diff.value(QStringLiteral("documents")).toArray()) {
        const QJsonObject document = value.toObject();
        auto *group = new QTreeWidgetItem(files, {document.value(QStringLiteral("target")).toString()});
        const QString message = document.value(QStringLiteral("error")).toString(document.value(QStringLiteral("diagnostic")).toString());
        group->setData(0, Qt::UserRole + 1, message);
        for (const auto &entry : document.value(QStringLiteral("files")).toArray()) {
            const QJsonObject file = entry.toObject();
            auto *item = new QTreeWidgetItem(group, {file.value(QStringLiteral("path")).toString(), file.value(QStringLiteral("change")).toString()});
            item->setData(0, Qt::UserRole, file);
            item->setData(0, Qt::UserRole + 1, message);
            if (!initial || file.value(QStringLiteral("is_diagnostic")).toBool()) initial = item;
        }
        if (!group->childCount()) group->setText(1, document.contains(QStringLiteral("error")) ? i18n("Unavailable") : i18n("No source changes"));
        group->setExpanded(true);
    }
    connect(files, &QTreeWidget::currentItemChanged, &dialog, [open, snapshot, diagnostic](QTreeWidgetItem *item) {
        open->setEnabled(item && !item->data(0, Qt::UserRole).toJsonObject().isEmpty());
        snapshot->setEnabled(open->isEnabled());
        diagnostic->setText(item ? item->data(0, Qt::UserRole + 1).toString() : QString());
    });
    open->setEnabled(false);
    snapshot->setEnabled(false);
    files->setCurrentItem(initial ? initial : files->topLevelItem(0));
    const auto openComparison = [this, files, buildId, &dialog](bool editable) {
        const auto *item = files->currentItem();
        if (!item) return;
        const QJsonObject file = item->data(0, Qt::UserRole).toJsonObject();
        const QString storedPath = file.value(QStringLiteral("path")).toString();
        const QDir docsRoot(ShellUtils::docsRoot());
        const QString currentPath = QFileInfo(docsRoot.filePath(storedPath)).canonicalFilePath();
        const QString canonicalRoot = QFileInfo(docsRoot.absolutePath()).canonicalFilePath();
        if (editable && (QDir::isAbsolutePath(storedPath) || currentPath.isEmpty()
            || !currentPath.startsWith(canonicalRoot + QLatin1Char('/')) || !QFileInfo(currentPath).isFile())) {
            KMessageBox::error(&dialog, i18n("The current source file is unavailable. You can inspect the failed-build snapshot instead."));
            return;
        }
        const QString name = QFileInfo(storedPath).fileName();
        const QString root = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath(
            QStringLiteral("build-diffs/%1/%2").arg(buildId, QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QStringList paths;
        for (const QString &side : {QStringLiteral("before"), QStringLiteral("after")}) {
            const QString directory = QDir(root).filePath(side == QLatin1String("before") ? QStringLiteral("Last successful PDF") : QStringLiteral("Selected build"));
            if (!QDir().mkpath(directory)) { KMessageBox::error(&dialog, i18n("Could not save the source comparison.")); return; }
            const QString path = QDir(directory).filePath(name);
            QSaveFile output(path);
            const QByteArray content = file.value(side).toString().toUtf8();
            if (!output.open(QIODevice::WriteOnly) || !output.setPermissions(QFileDevice::ReadOwner)
                || output.write(content) != content.size() || !output.commit()) {
                KMessageBox::error(&dialog, i18n("Could not save the source comparison.")); return;
            }
            paths.append(path);
        }
        QString digest = file.value(QStringLiteral("after_sha256")).toString();
        if (digest.isEmpty()) digest = QString::fromLatin1(QCryptographicHash::hash(file.value(QStringLiteral("after")).toString().toUtf8(), QCryptographicHash::Sha256).toHex());
        openSourceComparison(editable ? currentPath : paths[1], qMax(1, file.value(QStringLiteral("line")).toInt()),
                             QString(), paths[0], editable ? digest : QString());
    };
    connect(open, &QPushButton::clicked, &dialog, [openComparison] { openComparison(true); });
    connect(snapshot, &QPushButton::clicked, &dialog, [openComparison] { openComparison(false); });
    connect(files, &QTreeWidget::itemDoubleClicked, open, [open] { if (open->isEnabled()) open->click(); });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}
