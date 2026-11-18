#include <QProgressDialog>
#include <QSettings>
#include <QActionGroup>
#include <QToolButton>
#include <functional>
#include "projectreadingcontext.h"
#include "projectfeedback.h"
#include "thoughtsplatform.h"
#include "thoughtshome.h"
#include "sourceowner.h"
#include "documentidentity.h"
#include "pdflinkoverridedialog.h"
#include "reviewcardfeedback.h"
#include "feedbackversion.h"
#include "pdfversions.h"
#include "outsidepdfcomments.h"
#include "reviewfeedbackoverlay.h"
#include "documentfeedback.h"
#include "documentfeedbackoverlay.h"
#include <QCache>
#include <QMutex>
#include <QMutexLocker>
#include "reviewplacement.h"
/*
    SPDX-FileCopyrightText: 2002 Wilco Greven <greven@kde.org>
    SPDX-FileCopyrightText: 2002 Chris Cheney <ccheney@cheney.cx>
    SPDX-FileCopyrightText: 2002 Malcolm Hunter <malcolm.hunter@gmx.co.uk>
    SPDX-FileCopyrightText: 2003-2004 Christophe Devriese <Christophe.Devriese@student.kuleuven.ac.be>
    SPDX-FileCopyrightText: 2003 Daniel Molkentin <molkentin@kde.org>
    SPDX-FileCopyrightText: 2003 Andy Goossens <andygoossens@telenet.be>
    SPDX-FileCopyrightText: 2003 Dirk Mueller <mueller@kde.org>
    SPDX-FileCopyrightText: 2003 Laurent Montel <montel@kde.org>
    SPDX-FileCopyrightText: 2004 Dominique Devriese <devriese@kde.org>
    SPDX-FileCopyrightText: 2004 Christoph Cullmann <crossfire@babylon2k.de>
    SPDX-FileCopyrightText: 2004 Henrique Pinto <stampede@coltec.ufmg.br>
    SPDX-FileCopyrightText: 2004 Waldo Bastian <bastian@kde.org>
    SPDX-FileCopyrightText: 2004-2008 Albert Astals Cid <aacid@kde.org>
    SPDX-FileCopyrightText: 2004 Antti Markus <antti.markus@starman.ee>

    Work sponsored by the LiMux project of the city of Munich:
    SPDX-FileCopyrightText: 2017 Klarälvdalens Datakonsult AB a KDAB Group company <info@kdab.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "part.h"
#include "docschangemonitor.h"

#include "config-okular.h"

// qt/kde includes
#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QStackedWidget>
#include <QContextMenuEvent>
#include <QCryptographicHash>
#include <QCursor>
#include <QDate>
#include <QDateTime>
#include <QDomDocument>
#if HAVE_DBUS
#include <QDBusConnection>
#endif // HAVE_DBUS
#include <QDialog>
#include "annotwindow.h"
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QMenuBar>
#include <QMetaMethod>
#include <QMimeData>
#include <QMimeDatabase>
#include <QMessageBox>
#include <QPrintDialog>
#include <QPrintPreviewDialog>
#include <QPrinter>
#include <QProcessEnvironment>
#include <QPromise>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSplitter>
#include <QSet>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTextStream>
#include <QThreadPool>
#include <QTime>
#include <QTimer>
#include <QUuid>
#include <QWidgetAction>

#include <KAboutPluginDialog>
#include <KActionCollection>
#include <KActionMenu>
#include <KBookmarkAction>
#include <KColorSchemeManager>
#include <KColorSchemeMenu>
#include <KCompressionDevice>
#include <KDirWatch>
#include <KFilterBase>
#include <KHamburgerMenu>
#include <KIO/FileCopyJob>
#include <KIO/Global>
#include <KIO/OpenFileManagerWindowJob>
#include <KIO/StatJob>
#include <KJobWidgets>
#include <KMainWindow>
#include <KMessageBox>
#include <KParts/GUIActivateEvent>
#include <KPasswordDialog>
#include <KPluginMetaData>
#include <KSharedDataCache>
#include <KStandardShortcut>
#include <KToggleAction>
#include <KToggleFullScreenAction>
#include <KToolBar>
#if HAVE_KWALLET
#include <KWallet>
#endif
#include <KXMLGUIClient>
#include <KXMLGUIFactory>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QLineEdit>

#if HAVE_PURPOSE
#include <Purpose/AlternativesModel>
#include <Purpose/Menu>
#endif

// local includes
#include "aboutdata.h"
#include "annotationmodel.h"
#include "commentsidebarstate.h"
#include "annotationpopup.h"
#include "bookmarklist.h"
#include "commentfragmentgroup.h"
#include "commentfragmentids.h"
#include "core/action.h"
#include "core/annotations.h"
#include "core/bookmarkmanager.h"
#include "core/document_p.h"
#include "core/form.h"
#include "core/generator.h"
#include "core/misc.h"
#include "core/page.h"
#include "core/printoptionswidget.h"
#include "core/sourcereference.h"
#include "core/utils.h"
#include "docscommentsynchronizer.h"
#include "docscommentmutationstore.h"
#include "drawingtoolactions.h"
#include "embeddedfilesdialog.h"
#include "extensions.h"
#include "fedorarepository.h"
#include "fileprinterpreview.h"
#include "findbar.h"
#include "gui/signatureguiutils.h"
#include "layers.h"
#include "minibar.h"
#include "okmenutitle.h"
#include "pagesizelabel.h"
#include "core/htmlattachment.h"
#include "htmlattachmentreader.h"
#include "htmldocumentview.h"
#include "htmledition.h"
#include "htmlreader.h"
#include "pageview.h"
#include "preferencesdialog.h"
#include "presentationwidget.h"
#include "propertiesdialog.h"
#include "settings.h"
#include "shell/agentproposalparser.h"
#include "agentproposaldecisionqueue.h"
#include "agentproposalhoverbar.h"
#include "shell/syncstatuslabels.h"
#include "side_reviews.h"
#include "sidebar.h"
#include "signaturepanel.h"
#include "thumbnaillist.h"
#include "toc.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

#ifdef OKULAR_KEEP_FILE_OPEN
class FileKeeper
{
public:
    FileKeeper()
        : m_handle(nullptr)
    {
    }

    ~FileKeeper()
    {
    }

    void open(const QString &path)
    {
        if (!m_handle)
            m_handle = std::fopen(QFile::encodeName(path).constData(), "r");
    }

    void close()
    {
        if (m_handle) {
            int ret = std::fclose(m_handle);
            Q_UNUSED(ret)
            m_handle = nullptr;
        }
    }

    QTemporaryFile *copyToTemporary() const
    {
        if (!m_handle)
            return nullptr;

        QTemporaryFile *retFile = new QTemporaryFile;
        retFile->open();

        std::rewind(m_handle);
        do {
            int c = std::fgetc(m_handle);
            if (c == EOF)
                break;
            if (!retFile->putChar((char)c))
                break;
        } while (!feof(m_handle));

        retFile->flush();

        return retFile;
    }

private:
    std::FILE *m_handle;
};
#endif

K_PLUGIN_FACTORY_WITH_JSON(OkularPartFactory, "okular_part.json", registerPlugin<Okular::Part>();)

static QAction *actionForExportFormat(const Okular::ExportFormat &format, QObject *parent = Q_NULLPTR)
{
    QAction *act = new QAction(format.description(), parent);
    if (!format.icon().isNull()) {
        act->setIcon(format.icon());
    }
    return act;
}

static QByteArray docsSourceFingerprint(const QString &path)
{
    QFile file(path);
    if (!file.exists()) {
        return QByteArrayLiteral("missing");
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArrayLiteral("unreadable:") + file.errorString().toUtf8();
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        return QByteArrayLiteral("unreadable:hash");
    }
    return hash.result();
}

constexpr int AgentProposalSearchId = 5;
constexpr int AgentProposalEndSearchId = 6;

static QString agentProposalVisibleText(QString proposalText)
{
    proposalText.replace(
        QRegularExpression(QStringLiteral("\\b(?:Table|Figure|Section|Appendix|Equation)\\s*~?\\s*\\\\(?:ref|pageref|eqref)\\s*\\{[^{}]*\\}"), QRegularExpression::CaseInsensitiveOption),
        QStringLiteral(" "));
    proposalText.replace(
        QRegularExpression(QStringLiteral("\\\\(?:label|ref|pageref|eqref|cite|citealp|citeauthor|citep|citet|input|include|includegraphics)\\s*(?:\\[[^\\]]*\\]\\s*)*\\{[^{}]*\\}")),
        QStringLiteral(" "));
    proposalText.replace(QRegularExpression(QStringLiteral("(?m)(?<!\\\\)%[^\\n]*")), QStringLiteral(" "));
    proposalText.replace(QStringLiteral("\\\\_"), QStringLiteral("_"));
    proposalText.replace(QStringLiteral("\\\\&"), QStringLiteral("&"));
    proposalText.replace(QStringLiteral("\\\\%"), QStringLiteral("%"));
    proposalText.replace(QStringLiteral("\\\\#"), QStringLiteral("#"));
    proposalText.replace(QStringLiteral("\\\\textbackslash"), QStringLiteral("\\"));
    proposalText.replace(QRegularExpression(QStringLiteral("\\\\(?:begin|end)\\s*\\{[^{}]*\\}")), QStringLiteral(" "));
    proposalText.replace(QRegularExpression(QStringLiteral("\\\\[A-Za-z@]+\\*?(?:\\[[^\\]]*\\])?")), QStringLiteral(" "));
    proposalText.replace(QRegularExpression(QStringLiteral("[{}$]")), QStringLiteral(" "));
    proposalText.replace(QLatin1Char('~'), QLatin1Char(' '));
    return proposalText.simplified();
}

static QString agentProposalHeadingSearchQuery(const QString &proposalText)
{
    const QRegularExpression headingExpression(
        QStringLiteral("\\\\(?:part|chapter|section|subsection|subsubsection|paragraph|subparagraph)\\*?\\s*(?:\\[[^\\]]*\\]\\s*)?\\{([^{}]+)\\}"));
    const QRegularExpressionMatch headingMatch = headingExpression.match(proposalText);
    if (headingMatch.hasMatch()) {
        QStringList headingWords = agentProposalVisibleText(headingMatch.captured(1)).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        while (headingWords.size() > 14) {
            headingWords.removeLast();
        }
        if (headingWords.size() >= 2) {
            return headingWords.join(QLatin1Char(' '));
        }
    }
    return {};
}

static QString agentProposalSearchQuery(const QString &proposalText)
{
    const QString headingQuery = agentProposalHeadingSearchQuery(proposalText);
    if (!headingQuery.isEmpty()) {
        return headingQuery;
    }
    QStringList words = agentProposalVisibleText(proposalText).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    while (words.size() > 14) {
        words.removeLast();
    }
    return words.size() >= 2 ? words.join(QLatin1Char(' ')) : QString();
}

static QStringList agentProposalEndSearchQueries(const QString &proposalText)
{
    const QStringList words = agentProposalVisibleText(proposalText).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QStringList queries;
    for (const int wordCount : {14, 10, 8, 6, 4, 3, 2}) {
        if (words.size() < wordCount) {
            continue;
        }
        const QString query = words.sliced(words.size() - wordCount).join(QLatin1Char(' '));
        if (!queries.contains(query)) {
            queries.append(query);
        }
    }
    if (queries.isEmpty() && words.size() >= 2) {
        queries.append(words.join(QLatin1Char(' ')));
    }
    const QString headingQuery = agentProposalHeadingSearchQuery(proposalText);
    if (!headingQuery.isEmpty() && !queries.contains(headingQuery)) {
        queries.append(headingQuery);
    }
    return queries;
}

static QStringList docsSourceProjects(const QString &docsRoot, const QStringList &sourceFiles)
{
    const QFileInfo rootInfo(docsRoot);
    const auto normalizedPath = [](QString path) {
        path = QDir::cleanPath(path);
#ifdef Q_OS_MACOS
        if (path.startsWith(QLatin1String("/private/var/"))) {
            path.remove(0, 8);
        }
#endif
        return path;
    };
    const QDir root(normalizedPath(rootInfo.absoluteFilePath()));
    QSet<QString> projects;
    for (const QString &sourceFile : sourceFiles) {
        const QString relative = root.relativeFilePath(normalizedPath(sourceFile));
        if (relative == QLatin1String("..") || relative.startsWith(QLatin1String("../"))) {
            continue;
        }
        const QString project = Thoughts::SourceOwner::of(relative);
        if (!project.isEmpty()) {
            projects.insert(project);
        }
    }
    QStringList sortedProjects(projects.begin(), projects.end());
    std::sort(sortedProjects.begin(), sortedProjects.end());
    return sortedProjects;
}

static KCompressionDevice::CompressionType compressionTypeFor(const QString &mime_to_check)
{
    // The compressedMimeMap is here in case you have a very old shared mime database
    // that doesn't have inheritance info for things like gzeps, etc
    // Otherwise the "is()" calls below are just good enough
    static QHash<QString, KCompressionDevice::CompressionType> compressedMimeMap;
    static bool supportBzip = false;
    static bool supportXz = false;
    static bool supportZstd = false;

    const QString app_gzip(QStringLiteral("application/x-gzip"));
    const QString app_bzip(QStringLiteral("application/x-bzip"));
    const QString app_xz(QStringLiteral("application/x-xz"));
    const QString app_zstd(QStringLiteral("application/zstd"));
    if (compressedMimeMap.isEmpty()) {
        std::unique_ptr<KFilterBase> f;
        compressedMimeMap[QStringLiteral("image/x-gzeps")] = KCompressionDevice::GZip;
        // check we can read bzip2-compressed files
        f.reset(KCompressionDevice::filterForCompressionType(KCompressionDevice::BZip2));
        if (f.get()) {
            supportBzip = true;
            compressedMimeMap[QStringLiteral("application/x-bzpdf")] = KCompressionDevice::BZip2;
            compressedMimeMap[QStringLiteral("application/x-bzpostscript")] = KCompressionDevice::BZip2;
            compressedMimeMap[QStringLiteral("application/x-bzdvi")] = KCompressionDevice::BZip2;
            compressedMimeMap[QStringLiteral("image/x-bzeps")] = KCompressionDevice::BZip2;
        }
        // check if we can read XZ-compressed files
        f.reset(KCompressionDevice::filterForCompressionType(KCompressionDevice::Xz));
        if (f.get()) {
            supportXz = true;
        }
        // check if we can read zstd-compressed files
        f.reset(KCompressionDevice::filterForCompressionType(KCompressionDevice::Zstd));
        if (f.get()) {
            supportZstd = true;
        }
    }
    QHash<QString, KCompressionDevice::CompressionType>::const_iterator it = compressedMimeMap.constFind(mime_to_check);
    if (it != compressedMimeMap.constEnd()) {
        return it.value();
    }

    QMimeDatabase db;
    QMimeType mime = db.mimeTypeForName(mime_to_check);
    if (mime.isValid()) {
        if (mime.inherits(app_gzip)) {
            return KCompressionDevice::GZip;
        } else if (supportBzip && mime.inherits(app_bzip)) {
            return KCompressionDevice::BZip2;
        } else if (supportXz && mime.inherits(app_xz)) {
            return KCompressionDevice::Xz;
        } else if (supportZstd && mime.inherits(app_zstd)) {
            return KCompressionDevice::Zstd;
        }
    }

    return KCompressionDevice::None;
}

static Okular::EmbedMode detectEmbedMode(QWidget *parentWidget, QObject *parent, const QVariantList &args)
{
    Q_UNUSED(parentWidget);

    if (parent && (parent->objectName().startsWith(QLatin1String("okular::Shell")) || parent->objectName().startsWith(QLatin1String("okular/okular__Shell")))) {
        return Okular::NativeShellMode;
    }

    for (const QVariant &arg : args) {
        if (arg.metaType().id() == QMetaType::QString) {
            if (arg.toString() == QLatin1String("Print/Preview")) {
                return Okular::PrintPreviewMode;
            } else if (arg.toString() == QLatin1String("ViewerWidget")) {
                return Okular::ViewerWidgetMode;
            }
        }
    }

    return Okular::UnknownEmbedMode;
}

static QString detectConfigFileName(const QVariantList &args)
{
    for (const QVariant &arg : args) {
        if (arg.metaType().id() == QMetaType::QString) {
            QString argString = arg.toString();
            int separatorIndex = argString.indexOf(QStringLiteral("="));
            if (separatorIndex >= 0 && argString.left(separatorIndex) == QLatin1String("ConfigFileName")) {
                return argString.mid(separatorIndex + 1);
            }
        }
    }

    return QString();
}

#undef OKULAR_KEEP_FILE_OPEN

#ifdef OKULAR_KEEP_FILE_OPEN
static bool keepFileOpen()
{
    static bool keep_file_open = !qgetenv("OKULAR_NO_KEEP_FILE_OPEN").toInt();
    return keep_file_open;
}
#endif

int Okular::Part::numberOfParts = 0;

// THOUGHTS_HOME lets tests construct a Part on a temporary Library.
static QString docsRenderRoot()
{
    return Thoughts::thoughtsHome();
}

namespace Okular
{
static QProcessEnvironment docsToolEnvironment();

Part::Part(QObject *parent, const QVariantList &args)
    : KParts::ReadWritePart(parent)
    , m_tempfile(nullptr)
    , m_documentOpenWithPassword(false)
    , m_swapInsteadOfOpening(false)
    , m_tocEnabled(false)
    , m_fileLastSize(-1)
    , m_ignoreDirtySignalsForCurrentRevision(false)
    , m_isReloading(false)
    , m_fileWasRemoved(false)
    , m_docsRenderRoot(docsRenderRoot())
    , m_fedoraRepository(FedoraRepository::instance(m_docsRenderRoot))
    , m_docsSyncTexExecutable()
    , m_docsDocumentDpi(72.0, 72.0)
    , m_docsSyncTexTimeoutMs(1000)
    , m_syncingDocsComments(false)
    , m_showMenuBarAction(nullptr)
    , m_showFullScreenAction(nullptr)
    , m_cliPresentation(false)
    , m_cliPrint(false)
    , m_cliPrintAndExit(false)
    , m_embedMode(detectEmbedMode(widget(), parent, args))
    , m_generatorGuiClient(nullptr)
    , m_keeper(nullptr)
{
    // make sure that the component name is okular otherwise the XMLGUI .rc files are not found
    // when this part is used in an application other than okular (e.g. unit tests)
    setComponentName(QStringLiteral("okular"), QString());
    m_docsCommentMutationStore = std::make_unique<DocsCommentMutationStore>(
        QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server")));

    setupConfigSkeleton(args);

#if HAVE_DBUS
    numberOfParts++;
    if (numberOfParts == 1) {
        m_registerDbusName = QStringLiteral("/okular");
    } else {
        m_registerDbusName = QStringLiteral("/okular%1").arg(numberOfParts);
    }
    QDBusConnection::sessionBus().registerObject(m_registerDbusName, this, QDBusConnection::ExportScriptableSlots);
#endif // HAVE_DBUS

    // connect the started signal to tell the job the mimetypes we like,
    // and get some more information from it
    connect(this, &KParts::ReadOnlyPart::started, this, &Part::slotJobStarted);

    // connect the completed signal so we can put the window caption when loading remote files
    connect(this, QOverload<>::of(&Part::completed), this, &Part::setWindowTitleFromDocument);
    connect(this, &KParts::ReadOnlyPart::canceled, this, &Part::loadCancelled);

    // create browser extension (for printing when embedded into browser)
    m_bExtension = new BrowserExtension(this);

    const QStringList iconDirs = QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, QStringLiteral("okular/pics"), QStandardPaths::LocateDirectory);
    QIcon::setFallbackSearchPaths(QIcon::fallbackSearchPaths() << iconDirs);

    m_sidebar = new Sidebar(widget());
    setWidget(m_sidebar);
    connect(m_sidebar, &Sidebar::urlsDropped, this, &Part::handleDroppedUrls);

    // build the document
    m_document = new Okular::Document(widget());
    m_document->setProperty("thoughtsCommentOwner", QVariant::fromValue<QObject *>(this));
    connect(m_document, &Document::linkFind, this, &Part::slotFind);
    connect(m_document, &Document::linkGoToPage, this, &Part::slotGoToPage);
    connect(m_document, &Document::linkPresentation, this, &Part::slotShowPresentation);
    connect(m_document, &Document::linkEndPresentation, this, &Part::slotHidePresentation);
    connect(m_document, &Document::openUrl, this, &Part::openUrlFromDocument);
    if (m_embedMode == NativeShellMode) {
        connect(m_document, &Document::openExternalDocument, this, &Part::requestOpenDocumentLink);
    }
    connect(m_document, &Document::navigationViewportChanged, this, &Part::navigationViewportChanged);
    connect(m_document->bookmarkManager(), &BookmarkManager::openUrl, this, &Part::openUrlFromBookmarks);
    connect(m_document, &Document::close, this, &Part::close);
    connect(m_document, &Document::requestPrint, this, &Part::slotPrint);
    connect(m_document, &Document::requestSaveAs, this, [this] {
        if (m_exportAsPdfWithComments) {
            m_exportAsPdfWithComments->trigger();
        }
    });
    connect(m_document, &Document::undoHistoryCleanChanged, this, [this](bool clean) {
        // A comment on a PDF outside the Library: saved on this computer a moment later.
        if (!clean && !m_outsideCommentsPdfPath.isEmpty()) m_outsideCommentsSaveTimer->start();
        const bool commentMutationPending = shouldSyncDocsComments(url()) && (!m_pendingDocsCommentMutationIds.isEmpty() || !m_transientEmptyDocsCommentIds.isEmpty());
        if (clean || !commentMutationPending) {
            setModified(!clean);
        }
        setWindowTitleFromDocument();
    });
    connect(m_document, &Document::annotationMutated, this, &Part::handleDocsAnnotationMutation);

    if (parent && parent->metaObject()->indexOfSlot(QMetaObject::normalizedSignature("slotQuit()").constData()) != -1) {
        connect(m_document, SIGNAL(quit()), parent, SLOT(slotQuit())); // clazy:exclude=old-style-connect
    } else {
        connect(m_document, &Document::quit, this, &Part::cannotQuit);
    }
    // widgets: ^searchbar (toolbar containing label and SearchWidget)
    //      m_searchToolBar = new KToolBar( parentWidget, "searchBar" );
    //      m_searchToolBar->boxLayout()->setSpacing( KDialog::spacingHint() );
    //      QLabel * sLabel = new QLabel( i18n( "&Search:" ), m_searchToolBar, "kde toolbar widget" );
    //      m_searchWidget = new SearchWidget( m_searchToolBar, m_document );
    //      sLabel->setBuddy( m_searchWidget );
    //      m_searchToolBar->setStretchableWidget( m_searchWidget );

    // Thoughts exposes Annotations and, when available, Contents. Keep the
    // remaining sidebar models alive in a hidden owner because document and
    // action logic still relies on them.
    QWidget *sidebarSupportWidgets = new QWidget(m_sidebar);
    sidebarSupportWidgets->hide();

    m_toc = new TOC(sidebarSupportWidgets, m_document);
    connect(m_toc.data(), &TOC::hasTOC, this, &Part::enableTOC);
    connect(m_toc.data(), &TOC::rightClick, this, &Part::slotShowTOCMenu);

    m_layers = new Layers(sidebarSupportWidgets, m_document);
    connect(m_layers.data(), &Layers::hasLayers, this, &Part::enableLayers);

    QWidget *thumbsBox = new ThumbnailsBox(sidebarSupportWidgets);
    thumbsBox->layout()->setSpacing(6);
    m_thumbnailList = new ThumbnailList(thumbsBox, m_document);
    thumbsBox->layout()->addWidget(m_thumbnailList);
    //	ThumbnailController * m_tc = new ThumbnailController( thumbsBox, m_thumbnailList );
    connect(m_thumbnailList.data(), &ThumbnailList::rightClick, this, &Part::slotShowMenu);

    m_reviewsWidget = new Reviews(nullptr, m_document, m_docsRenderRoot);
    connect(m_reviewsWidget, &Reviews::commentsDeleteRequested, this, [this](const QStringList &ids) {
        const auto result = deleteComments(ids);
        if (!result.value(QStringLiteral("success")).toBool()) KMessageBox::error(widget(), result.value(QStringLiteral("error")).toString());
    });
    connect(m_reviewsWidget, &Reviews::openCommentRequested, this, [this](const QString &id, int page, bool unplaced) {
        const int expectedPage = page >= 0 ? page + 1 : 0;
        if (unplaced) {
            openUnplacedComment(id, expectedPage);
        } else {
            openReviewAnnotation(id, expectedPage);
        }
    });
    connect(m_reviewsWidget, &Reviews::agentProposalsResolutionRequested, this, &Part::resolveAgentProposalsFromSidebar);
    connect(m_reviewsWidget, &Reviews::agentProposalReviewRequested, this, [this](const QString &proposalId) { requestAgentProposalReview(proposalId); });
    connect(m_reviewsWidget, &Reviews::agentProposalsChanged, this, [this] {
        if (!m_pageView) {
            return;
        }
        m_locatedAgentProposalReviewId.clear();
        if (m_pendingAgentProposalResolutions.isEmpty()) {
            const QString proposalId = m_pageView->openAgentProposalId();
            if (!proposalId.isEmpty()) {
                const AgentProposalSidebarEntry proposal = m_reviewsWidget->agentProposal(proposalId);
                if (proposal.id == proposalId) {
                    const bool proposedTextIsEmpty = proposal.after.trimmed().isEmpty();
                    m_currentAgentProposalText = proposedTextIsEmpty ? proposal.before : proposal.after;
                    m_currentAgentProposalFallbackText = proposedTextIsEmpty ? QString() : proposal.before;
                    m_currentAgentProposalSourcePath = proposal.sourcePath;
                    m_currentAgentProposalSourceLine = proposal.sourceLine;
                    m_currentAgentProposalTargetsCurrentText = proposedTextIsEmpty;
                }
                m_pageView->refreshAgentProposalWindow(proposal);
            }
        }
        m_pageView->setDecidedAgentProposalIds(m_reviewsWidget->decidedAgentProposalIds());
        m_pageView->refreshAgentProposalOverlays();
    });
    m_sidebar->addItem(m_reviewsWidget, QIcon::fromTheme(QStringLiteral("draw-freehand")), i18n("Annotations"));
    m_sidebar->setCurrentItem(m_reviewsWidget);

    m_bookmarkList = new BookmarkList(m_document, sidebarSupportWidgets);

    m_signaturePanel = new SignaturePanel(m_document, sidebarSupportWidgets);
    connect(m_signaturePanel.data(), &SignaturePanel::documentHasSignatures, this, &Part::enableSidebarSignaturesItem);

    // widgets: [] | [right 'pageView']
    QWidget *rightContainer = new QWidget(nullptr);
    m_sidebar->setMainWidget(rightContainer);
    QVBoxLayout *rightLayout = new QVBoxLayout(rightContainer);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);
    //	KToolBar * rtb = new KToolBar( rightContainer, "mainToolBarSS" );
    //	rightLayout->addWidget( rtb );
    m_migrationMessage = new KMessageWidget(rightContainer);
    m_migrationMessage->setVisible(false);
    m_migrationMessage->setWordWrap(true);
    m_migrationMessage->setMessageType(KMessageWidget::Warning);
    m_migrationMessage->setPosition(KMessageWidget::Position::Header);
    m_migrationMessage->setText(i18n("This document contains legacy annotations or form data. Thoughts opens PDF content read-only; use Export As to create a separate copy."));
    rightLayout->addWidget(m_migrationMessage);
    m_topMessage = new KMessageWidget(rightContainer);
    m_topMessage->setVisible(false);
    m_topMessage->setWordWrap(true);
    m_topMessage->setMessageType(KMessageWidget::Information);
    m_topMessage->setPosition(KMessageWidget::Position::Header);
    m_topMessage->setText(i18n("This document has embedded files. <a href=\"okular:/embeddedfiles\">Click here to see them</a> or go to File -> Embedded Files."));
    m_topMessage->setIcon(QIcon::fromTheme(QStringLiteral("mail-attachment")));
    connect(m_topMessage, &KMessageWidget::linkActivated, this, &Part::slotShowEmbeddedFiles);
    rightLayout->addWidget(m_topMessage);
    m_formsMessage = new KMessageWidget(rightContainer);
    m_formsMessage->setVisible(false);
    m_formsMessage->setWordWrap(true);
    m_formsMessage->setMessageType(KMessageWidget::Information);
    m_formsMessage->setPosition(KMessageWidget::Position::Header);
    rightLayout->addWidget(m_formsMessage);
    m_infoMessage = new KMessageWidget(rightContainer);
    m_infoMessage->setVisible(false);
    m_infoMessage->setWordWrap(true);
    m_infoMessage->setMessageType(KMessageWidget::Information);
    m_infoMessage->setPosition(KMessageWidget::Position::Header);
    rightLayout->addWidget(m_infoMessage);
    m_infoTimer = new QTimer();
    m_infoTimer->setSingleShot(true);
    connect(m_infoTimer, &QTimer::timeout, m_infoMessage, &KMessageWidget::animatedHide);
    m_agentProposalMessage = new KMessageWidget(rightContainer);
    m_agentProposalMessage->setVisible(false);
    m_agentProposalMessage->setWordWrap(true);
    m_agentProposalMessage->setMessageType(KMessageWidget::Information);
    m_agentProposalMessage->setPosition(KMessageWidget::Position::Header);
    rightLayout->addWidget(m_agentProposalMessage);
    m_signatureMessage = new KMessageWidget(rightContainer);
    m_signatureMessage->setVisible(false);
    m_signatureMessage->setWordWrap(true);
    m_signatureMessage->setPosition(KMessageWidget::Position::Header);
    rightLayout->addWidget(m_signatureMessage);
#if HAVE_NEW_SIGNATURE_API
    m_signatureInProgressMessage = new KMessageWidget(rightContainer);
    m_signatureInProgressMessage->setCloseButtonVisible(false);
    m_signatureInProgressMessage->setVisible(false);
    m_signatureInProgressMessage->setWordWrap(true);
    m_signatureInProgressMessage->setText(i18n("Signing in progress. You can adjust the position and size of the signature"));
    m_signatureInProgressMessage->setPosition(KMessageWidget::Position::Header);
    QAction *finishSigningAction = new QAction(QIcon::fromTheme(QStringLiteral("dialog-ok-apply")), i18nc("@action:button finish the signing process", "Finish Signing"), this);
    connect(finishSigningAction, &QAction::triggered, this, &Part::finishSigning);
    m_signatureInProgressMessage->addAction(finishSigningAction);
    rightLayout->addWidget(m_signatureInProgressMessage);
#endif
    m_pageView = new PageView(rightContainer, m_document);
    m_pageView->setDocsRoot(m_docsRenderRoot);
    m_pageView->setCommentRecordProvider([this](const QString &id) { return currentCommentDetailsRecord(id); });
    m_pageView->setLinkOverrideHandlers([this](const Okular::Action *action, int page) { return openPdfLinkOverride(action, page); },
                                        [this](QMenu *menu, const Okular::Action *action, int page) { addPdfLinkOverrideActions(menu, action, page); });
    connect(m_pageView, &PageView::agentProposalOverlaysReloaded, this, &Part::locateOpenAgentProposal);
    connect(m_pageView, &PageView::reviewFeedbackChanged, this, &Part::reviewFeedbackChanged);
    m_pageView->reviewFeedback()->setWriter([this](const QJsonObject &record, QString *error) {
        if (!m_fedoraRepository) {
            if (error) *error = i18n("The local sync queue is not available.");
            return false;
        }
        return m_fedoraRepository->enqueueReviewCardFeedback(record, error);
    });
    m_pageView->documentFeedback()->setWriter([this](const QJsonObject &record, QString *error) {
        if (!m_fedoraRepository) {
            if (error) *error = i18n("The local sync queue is not available.");
            return false;
        }
        const auto own = projectFeedbackToDocument(record, error);
        return own && m_fedoraRepository->enqueueDocumentFeedback(*own, error);
    });
    m_pageView->reviewFeedback()->setBatchWriter([this](const QList<QJsonObject> &records, QString *error) {
        if (!m_fedoraRepository) {
            if (error) *error = i18n("The local sync queue is not available.");
            return false;
        }
        return m_fedoraRepository->enqueueReviewCardFeedbackBatch(records, error);
    });
    m_pageView->documentFeedback()->setBatchWriter([this](const QList<QJsonObject> &records, QString *error) {
        if (!m_fedoraRepository) {
            if (error) *error = i18n("The local sync queue is not available.");
            return false;
        }
        QList<QJsonObject> own;
        for (const QJsonObject &record : records) {
            const auto translated = projectFeedbackToDocument(record, error);
            if (!translated) return false;
            own.append(*translated);
        }
        return m_fedoraRepository->enqueueDocumentFeedbackBatch(own, error);
    });
    connect(m_reviewsWidget, &Reviews::replyToAnnotation, m_pageView, &PageView::promptReplyToAnnotation);
    rightContainer->setFocusProxy(m_pageView);
    QMetaObject::invokeMethod(m_pageView, "setFocus", Qt::QueuedConnection); // usability setting
    //    m_splitter->setFocusProxy(m_pageView);
    connect(m_pageView.data(), &PageView::rightClick, this, &Part::slotShowMenu);
    connect(m_pageView.data(), &PageView::contextMenuReady, this, &Part::addReadingActions);
    connect(m_pageView.data(), &PageView::documentFeedbackChanged, this, [this] {
        showDocumentFeedbackList();
    });
    connect(m_pageView.data(), &PageView::documentFeedbackAdded, this, [this](const QString &feedbackId) {
        if (showAnnotationsForNewEntry()) m_reviewsWidget->selectFeedback(feedbackId);
    });
    connect(m_pageView.data(), &PageView::annotationAdded, this, [this](Okular::Annotation *annotation, int pageNumber) {
        if (showAnnotationsForNewEntry()) m_reviewsWidget->selectAnnotation(annotation, pageNumber);
    });
    connect(m_reviewsWidget.data(), &Reviews::feedbackRequested, this, &Part::showFeedback);
    connect(m_reviewsWidget.data(), &Reviews::feedbackDeleteRequested, m_pageView.data(), &PageView::deleteDocumentFeedback);
    connect(m_reviewsWidget.data(), &Reviews::feedbackResolveRequested, m_pageView.data(), &PageView::resolveDocumentFeedback);
    connect(m_reviewsWidget.data(), &Reviews::earlierFeedbackToggled, this, [this](bool shown) { m_pageView->documentFeedback()->setShowEarlier(shown); });
    m_pageView->documentFeedback()->setAttachmentVersions([this](const QString &path) { return attachmentVersion(path); });
    connect(m_pageView.data(), &PageView::commentRelatedMetadataSaveRequested, this, [this](const QString &annotationId, int page, const QString &contents, const QVariantMap &metadataPatch) {
        const QVariantMap result = editReviewAnnotation(annotationId, page, contents, metadataPatch);
        m_pageView->completeAnnotationRelatedOperation(annotationId, result);
    });
    connect(m_pageView.data(), &PageView::commentThreadActionSaveRequested, this, [this](const QString &rootId, bool needsAction) {
        const bool success = saveUnplacedComment(rootId, {{QStringLiteral("needs_action"), needsAction}});
        m_pageView->completeCommentThreadAction(rootId, success);
    });
    connect(m_pageView.data(), &PageView::commentReplyRequested, this, [this](const QString &annotationId, int page, const QString &contents) {
        m_pageView->completeAnnotationReplyOperation(annotationId, addReviewAnnotationReply(annotationId, page, contents));
    });
    connect(m_pageView.data(), &PageView::commentRelatedChatGptUrlSaveRequested, this, [this](const QString &annotationId, const QString &url, bool openAfterSave) {
        QVariantMap result = updateReviewAnnotationChatGptUrl(annotationId, url);
        result.insert(QStringLiteral("openChatGpt"), openAfterSave && result.value(QStringLiteral("success")).toBool());
        m_pageView->completeAnnotationRelatedOperation(annotationId, result);
    });
    connect(m_pageView.data(), &PageView::annotationWindowOpening, this, [this] { delete m_unplacedCommentPanel.data(); });
    connect(m_pageView.data(), &PageView::commentRelatedOpenCodexRequested, this, &Part::requestOpenCommentCodex);
    connect(m_pageView.data(), &PageView::commentRelatedOpenChatGptRequested, this, &Part::requestOpenCommentChatGpt);
    connect(m_pageView.data(), &PageView::commentRelatedOpenSourceRequested, this, &Part::requestOpenCommentSource);
    connect(m_pageView.data(), &PageView::agentProposalReviewRequested, this, [this](const QString &proposalId) { requestAgentProposalReview(proposalId); });
    connect(m_pageView.data(), &PageView::agentProposalResolutionRequested, this, &Part::resolveAgentProposalFromCard);
    m_pageView->setAgentProposalDecisionStateProvider([this](const QString &proposalId) { return agentProposalDecisionState(proposalId); });
    m_liveProposalDecisions = new Thoughts::LiveProposalDecisionQueue(this);
    connect(m_liveProposalDecisions, &Thoughts::LiveProposalDecisionQueue::finished, this, &Part::finishLiveAgentProposalDecision);
    connect(m_pageView.data(), &PageView::agentProposalOpenSourceRequested, this, &Part::requestOpenCommentSource);
    connect(m_document, &Document::searchFinished, this, [this](int searchId, Document::SearchStatus status) {
        if (searchId == AgentProposalSearchId) {
            finishAgentProposalTextSearch(status);
        }
    });
    connect(m_pageView.data(), &PageView::openTexSourceRequested, this, &Part::openTexSource);
    connect(m_pageView.data(), &PageView::manualReviewCardRequested, this, &Part::requestManualReviewCard);
    connect(m_pageView.data(), &PageView::manualReviewCardAreasRequested, this, &Part::requestManualReviewCardAreas);
    connect(m_pageView.data(), &PageView::sectionReviewCardRequested, this, &Part::requestSectionReviewCard);
    connect(m_pageView.data(), &PageView::readAloudDeveloperEvent, this, &Part::developerEvent);
    connect(m_pageView.data(), &PageView::commentRelatedOpenOverviewRequested, this, &Part::requestCommentsOverviewForComment);
    connect(m_pageView, &PageView::triggerSearch, this, [this](const QString &searchText) {
        slotShowFindBar();
        m_findBar->startSearch(searchText);
    });
    connect(m_document, &Document::error, this, &Part::errorMessage);
    connect(m_document, &Document::warning, this, &Part::warningMessage);
    connect(m_document, &Document::notice, this, &Part::noticeMessage);
    connect(m_document, &Document::sourceReferenceActivated, this, &Part::slotHandleActivatedSourceReference);
    connect(m_pageView.data(), &PageView::fitWindowToPage, this, &Part::fitWindowToPage);
    // The page view or, switched in its place, the HTML view (setupHtmlView).
    m_pageSplitter = new QSplitter(Qt::Horizontal, rightContainer);
    m_pageSplitter->setObjectName(QStringLiteral("thoughtsPageSplitter"));
    m_pageSplitter->setChildrenCollapsible(false);
    setupHtmlView(m_pageSplitter);
    m_pageSplitter->addWidget(m_documentStack);
    rightLayout->addWidget(m_pageSplitter);
    m_layers->setPageView(m_pageView);
    m_signaturePanel->setPageView(m_pageView);
    m_findBar = new FindBar(m_document);
    m_sidebar->setTransientWidget(m_findBar);
    m_bottomBar = new QWidget(rightContainer);
    QHBoxLayout *bottomBarLayout = new QHBoxLayout(m_bottomBar);
    m_pageSizeLabel = new PageSizeLabel(m_bottomBar, m_document);
    bottomBarLayout->setContentsMargins(0, 0, 0, 0);
    bottomBarLayout->setSpacing(0);
    bottomBarLayout->addItem(new QSpacerItem(5, 5, QSizePolicy::Expanding, QSizePolicy::Minimum));
    m_miniBarLogic = new MiniBarLogic(this, m_document);
    m_miniBar = new MiniBar(m_bottomBar, m_miniBarLogic);
    bottomBarLayout->addWidget(m_miniBar);
    bottomBarLayout->addWidget(m_pageSizeLabel);
    rightLayout->addWidget(m_bottomBar);

    m_pageNumberTool = new MiniBar(nullptr, m_miniBarLogic);

    connect(m_findBar, &FindBar::onCloseButtonPressed, this, &Part::slotHideFindBar);
    connect(m_miniBar, &MiniBar::forwardKeyPressEvent, m_pageView, &PageView::externalKeyPressEvent);
    connect(m_pageNumberTool, &MiniBar::forwardKeyPressEvent, m_pageView, &PageView::externalKeyPressEvent);
    connect(m_pageView.data(), &PageView::requestOpenNewlySignedFile, this, &Part::requestOpenNewlySignedFile);
    connect(m_pageView.data(), &PageView::commentEditorClosing, this, &Part::discardTransientEmptyDocsComment);

    connect(m_reviewsWidget.data(), &Reviews::openAnnotationWindow, m_pageView.data(), &PageView::openAnnotationWindow);
    connect(m_reviewsWidget.data(), &Reviews::closeReviewAnnotationWindow, this, [this] {
        m_pageView->closeReviewAnnotationWindow();
        if (m_unplacedCommentPanel) {
            m_unplacedCommentPanel->close();
        }
    });
    connect(m_reviewsWidget.data(), &Reviews::openReviewAnnotationWindow, m_pageView.data(), &PageView::openReviewAnnotationWindow);
    connect(m_reviewsWidget.data(), &Reviews::reviewSelectionChanged, m_pageView.data(), &PageView::setReviewSelection);
    connect(m_reviewsWidget.data(), &Reviews::reviewCardHighlightsChanged, m_pageView.data(), &PageView::setReviewCardHighlights);
    connect(m_reviewsWidget.data(), &Reviews::reviewCardPresentationRequested, this, [this](const QVariantList &regions, bool navigate) {
        m_pageView->closeReviewAnnotationWindow();
        m_pageView->showReviewMasks(regions, PageView::ReviewMaskStyle::SidebarHighlight, navigate);
    });
    connect(m_reviewsWidget.data(), &Reviews::clearReviewCardPresentationRequested, m_pageView.data(), &PageView::clearReviewMask);
    connect(m_reviewsWidget.data(), &Reviews::reviewCardHighlightHidden, m_pageView.data(), &PageView::clearSidebarReviewHighlight);
    connect(m_reviewsWidget.data(), &Reviews::reviewCardEditRequested, this, &Part::requestEditReviewCard);
    connect(m_reviewsWidget.data(), &Reviews::reviewCardOpenRequested, this, &Part::requestOpenReviewCard);
    connect(m_reviewsWidget.data(), &Reviews::reviewCardReviewRequested, this, &Part::requestReviewReviewCard);
    connect(m_reviewsWidget.data(), &Reviews::reviewCardsMergeRequested, this, &Part::requestMergeReviewCards);
    connect(m_reviewsWidget.data(), &Reviews::reviewCardKindRequested, this, &Part::requestReviewCardKind);
    connect(m_reviewsWidget.data(), &Reviews::reviewCardsRetireRequested, this, &Part::requestRetireReviewCards);
    connect(m_reviewsWidget.data(), &Reviews::reviewCardsDeleteRequested, this, &Part::requestDeleteReviewCards);
    // Showing or hiding a kind in the Annotations list also shows or hides its
    // markup on the page. Go through the Visible markup toggle so the setting is
    // saved and every open document follows, exactly as from the toolbar menu.
    // The Review cards toggle is only the general layer of all card answers; a
    // review session's own answer mask is drawn independently of it.
    connect(m_reviewsWidget.data(), &Reviews::typeFilterClicked, this, [this](int itemType, bool shown) {
        QString name;
        switch (itemType) {
        case AnnotationModel::Comment:
            name = QStringLiteral("highlight_comments");
            break;
        case AnnotationModel::Feedback:
            name = QStringLiteral("highlight_feedback");
            break;
        case AnnotationModel::AgentProposal:
            name = QStringLiteral("highlight_agent_edits");
            break;
        case AnnotationModel::ReviewCard:
            name = QStringLiteral("highlight_review_cards");
            break;
        default:
            return;
        }
        if (QAction *toggle = actionCollection()->action(name)) {
            toggle->setChecked(shown);
        }
    });
    connect(m_pageView.data(), &PageView::reviewCardEditRequested, m_reviewsWidget.data(), &Reviews::editActiveReviewCard);
    connect(m_pageView.data(), &PageView::annotationClicked, m_reviewsWidget.data(), &Reviews::selectAnnotation);

    // add document observers
    m_document->addObserver(this);
    m_document->addObserver(m_thumbnailList);
    m_document->addObserver(m_pageView);
    m_document->registerView(m_pageView);
    m_document->addObserver(m_toc);
    m_document->addObserver(m_miniBarLogic);
    m_document->addObserver(m_reviewsWidget);
    m_document->addObserver(m_pageSizeLabel);
    m_document->addObserver(m_bookmarkList);
    m_document->addObserver(m_signaturePanel);

    connect(m_document->bookmarkManager(), &BookmarkManager::saved, this, &Part::slotRebuildBookmarkMenu);

    setupViewerActions();
#if HAVE_NEW_SIGNATURE_API
    connect(m_pageView.data(), &PageView::signingStarted, this, [this] {
        m_signatureInProgressMessage->setVisible(true);
    });
#endif

    if (m_embedMode != ViewerWidgetMode) {
        setupActions();
    } else {
        setViewerShortcuts();
    }

    // document watcher and reloader
    m_watcher = new KDirWatch(this);
    connect(m_watcher, &KDirWatch::dirty, this, &Part::slotFileDirty);
    connect(m_watcher, &KDirWatch::created, this, &Part::slotFileDirty);
    connect(m_watcher, &KDirWatch::deleted, this, &Part::slotFileDirty);
    m_dirtyHandler = new QTimer(this);
    m_dirtyHandler->setSingleShot(true);
    connect(m_dirtyHandler, &QTimer::timeout, this, [this] { slotAttemptReload(); });
    m_docsCommentRefreshTimer = new QTimer(this);
    m_docsCommentRefreshTimer->setSingleShot(true);
    m_docsCommentRefreshTimer->setInterval(80);
    m_docsCommentLoadWatcher = new QFutureWatcher<QJsonObject>(this);
    connect(m_docsCommentRefreshTimer, &QTimer::timeout, this, &Part::startDocsCommentLoad);
    connect(m_docsCommentLoadWatcher, &QFutureWatcher<QJsonObject>::finished, this, &Part::finishDocsCommentLoad);
    m_docsSourceReconcileTimer = new QTimer(this);
    m_docsSourceReconcileTimer->setSingleShot(true);
    m_docsSourceReconcileTimer->setInterval(160);
    followDocsChanges();
    m_docsCommentMutationCaptureTimer = new QTimer(this);
    m_docsCommentMutationCaptureTimer->setSingleShot(true);
    m_outsideCommentsSaveTimer = new QTimer(this);
    m_outsideCommentsSaveTimer->setSingleShot(true);
    m_outsideCommentsSaveTimer->setInterval(300);
    connect(m_outsideCommentsSaveTimer, &QTimer::timeout, this, [this] { saveOutsidePdfComments(); });
    m_docsCommentMutationFlushTimer = new QTimer(this);
    m_docsCommentMutationFlushTimer->setSingleShot(true);
    m_docsCommentMutationFlushTimer->setInterval(100);
    m_docsPdfIdentityWatcher = new QFutureWatcher<QJsonObject>(this);
    m_docsCommentMutationFlushWatcher = new QFutureWatcher<DocsCommentMutationFlushResult>(this);
    m_docsSourceReconcileWatcher = new QFutureWatcher<QJsonObject>(this);
    connect(m_docsSourceReconcileTimer, &QTimer::timeout, this, &Part::reconcileDocsSourceChanges);
    connect(m_docsSourceReconcileWatcher, &QFutureWatcher<QJsonObject>::finished, this, &Part::finishDocsSourceReconciliation);
    connect(m_docsCommentMutationCaptureTimer, &QTimer::timeout, this, &Part::capturePendingDocsCommentMutations);
    connect(m_docsCommentMutationFlushTimer, &QTimer::timeout, this, &Part::startDocsCommentMutationFlush);
    connect(m_docsPdfIdentityWatcher, &QFutureWatcher<QJsonObject>::finished, this, &Part::finishDocsPdfIdentityVerification);
    connect(m_docsCommentMutationFlushWatcher, &QFutureWatcher<DocsCommentMutationFlushResult>::finished, this, &Part::finishDocsCommentMutationFlush);
    connect(m_fedoraRepository, &FedoraRepository::projectionsChanged, this, [this](const QJsonObject &projections, qint64 frontier) {
        Q_EMIT fedoraProjectionsChanged(projections, frontier);
        m_fedoraProjections = projections;
        m_hasFedoraProjectionSnapshot = true;
        if (m_pageView && m_pageView->reviewFeedback()->hasCard())
            m_pageView->reviewFeedback()->setRecords(Thoughts::ReviewCardFeedback::recordsForCard(projections, m_pageView->reviewFeedback()->cardId(),
                                                                                                  m_pageView->reviewFeedback()->document(),
                                                                                                  m_pageView->reviewFeedback()->homeCards(), true));
        if (m_pageView && m_pageView->documentFeedback()->hasDocument()) {
            if (m_projectFeedback.reading) refreshDocumentFeedback();
            else m_pageView->documentFeedback()->setRecords(Thoughts::DocumentFeedback::recordsOfDocument(projections, m_pageView->documentFeedback()->documentId(), true));
        }
        if (m_htmlReader && m_htmlReader->isActive()) {
            refreshHtmlComments();
            refreshHtmlFeedback();
        }
        if (m_syncUiDeferred) { return; }
        if (m_pageView) {
            m_pageView->refreshOpenAnnotationRelatedInfo();
            if (auto *panel = qobject_cast<AnnotWindow *>(m_unplacedCommentPanel.data())) {
                panel->setThreadActionInfo(m_pageView->annotationRelatedInfo(panel->property("commentId").toString()));
            }
        }
        scheduleDocsCommentRefresh();
    });
    connect(m_fedoraRepository, &FedoraRepository::pendingChangesChanged, this, &Part::pendingChangesChanged);
    connect(m_fedoraRepository,
            &FedoraRepository::agentProposalResolutionStatusChanged,
            this,
            &Part::handleAgentProposalResolutionStatus);
    // The repository's durable projection snapshot is available before a Part
    // connects to its change signal. Seed from it so the companion fallback
    // does not repeatedly parse the same sidecar while waiting for a change.
    m_fedoraProjections = m_fedoraRepository->effectiveProjections();
    m_hasFedoraProjectionSnapshot = m_fedoraRepository->hasProjectionSnapshot();
    connect(m_fedoraRepository, &FedoraRepository::libraryGenerationWillActivate, this, [this] {
        m_libraryActivationPdfFilename.clear();
        if (!url().isLocalFile()) {
            return;
        }
        const QFileInfo documentInfo(localFilePath());
        const auto identity = Thoughts::DocumentIdentityStore(m_docsRenderRoot).forFile(documentInfo.absoluteFilePath());
        if (identity.isEmpty()) return;
        m_libraryActivationPdfFilename = identity.value(QStringLiteral("filename")).toString();
        m_libraryActivationDocumentId = identity.value(QStringLiteral("document_id")).toString();
        m_libraryActivationPdfSha256 = identity.value(QStringLiteral("pdf_sha256")).toString();
        unsetFileToWatch();
        m_libraryActivationWatchSuspended = true;
    });
    connect(m_fedoraRepository, &FedoraRepository::libraryGenerationActivated, this, [this](const QStringList &, bool success) {
        if (!m_libraryActivationWatchSuspended) {
            return;
        }
        m_libraryActivationWatchSuspended = false;
        const QString filename = std::exchange(m_libraryActivationPdfFilename, QString());
        if (filename.isEmpty()) {
            return;
        }
        if (!success) {
            const QString path = localFilePath();
            if (QFileInfo::exists(path)) {
                setFileToWatch(path);
            }
            return;
        }

        const QString currentRoot = QDir(m_docsRenderRoot).filePath(QStringLiteral("current_pdfs"));
        const QString currentPath = QDir(currentRoot).filePath(filename);
        if (!QFileInfo(currentPath).isFile()) {
            return;
        }
        const auto published = Thoughts::DocumentIdentityStore(m_docsRenderRoot).forFile(currentPath);
        if (published.value(QStringLiteral("document_id")).toString() != m_libraryActivationDocumentId) {
            setFileToWatch(localFilePath());
            return;
        }
        if (published.value(QStringLiteral("pdf_sha256")).toString() == m_libraryActivationPdfSha256
            && m_document->relocateUnchangedBackingFile(currentPath, QUrl::fromLocalFile(currentPath))) {
            // The loaded bytes and selection remain valid; only storage moved.
            setUrl(QUrl::fromLocalFile(currentPath));
            setLocalFilePath(currentPath);
            m_docsCommentPdfPath = currentPath;
            m_docsSourcePdfPath = currentPath;
            m_docsOpenedPdfPath = QFileInfo(currentPath).canonicalFilePath();
            scheduleDocsCommentRefresh();
            setFileToWatch(currentPath);
            return;
        }

        // A Library tab may have been opened through the canonical immutable
        // stage path. Reopen through current_pdfs so this and later atomic
        // generation changes keep one durable document identity while the
        // ordinary reload path preserves the viewport and sidebar.
        if (!slotAttemptReload(true, QUrl::fromLocalFile(currentPath))) {
            const QString path = localFilePath();
            if (QFileInfo::exists(path)) {
                setFileToWatch(path);
            }
        }
    });
    connect(m_fedoraRepository, &FedoraRepository::libraryImportFinished, this, &Part::libraryImportFinished);
    connect(m_fedoraRepository, &FedoraRepository::libraryDisplayNameFinished, this, &Part::libraryDisplayNameFinished);
    connect(m_fedoraRepository, &FedoraRepository::libraryRemovalFinished, this, &Part::libraryRemovalFinished);
    m_syncUiDeferred = m_fedoraRepository->manualSyncState() == FedoraRepository::ManualSyncState::Running;
    connect(m_fedoraRepository, &FedoraRepository::manualSyncStateChanged, this, [this](FedoraRepository::ManualSyncState state, const QString &message, const QString &details) {
        m_syncUiDeferred = state == FedoraRepository::ManualSyncState::Running;
        if (!m_syncUiDeferred) {
            const bool reloadPending = std::exchange(m_syncPdfReloadPending, false);
            const QUrl source = std::exchange(m_syncPdfReloadSource, {});
            const QUrl target = std::exchange(m_syncPdfReloadTarget, {});
            if (reloadPending && url() == source) {
                slotAttemptReload(true, target);
            }
            scheduleDocsCommentRefresh();
        }
        Q_EMIT fedoraManualSyncStateChanged(static_cast<int>(state), message, details);
    });
    // A build that a Sync left running on the server has ended: what checkFedoraRemoteWork would
    // answer, said without having been asked.
    connect(m_fedoraRepository, &FedoraRepository::remoteWorkFound, this, &Part::fedoraRemoteWorkFound);
    connect(m_fedoraRepository, &FedoraRepository::syncProgressChanged, this, &Part::syncProgressChanged);
    connect(m_fedoraRepository, &FedoraRepository::syncTransferProgressChanged, this, &Part::syncTransferProgressChanged);
    connect(m_fedoraRepository, &FedoraRepository::syncJournalEvent, this, &Part::syncJournalEvent);
    connect(m_fedoraRepository, &FedoraRepository::renderStatusChanged, this, &Part::fedoraRenderStatusChanged);
    connect(m_fedoraRepository, &FedoraRepository::renderStatusChanged, this, [this](const QString &target, const QJsonObject &status) {
        if (!m_agentProposalUndoState.available || m_agentProposalUndoState.action != QLatin1String("disregard")
            || (!m_agentProposalUndoState.target.isEmpty() && target != QLatin1String("all") && target != m_agentProposalUndoState.target)) {
            return;
        }
        const QString state = status.value(QStringLiteral("status")).toString();
        if (state == QLatin1String("attention") || state == QLatin1String("error") || state == QLatin1String("failed") || state == QLatin1String("cancelled")) {
            showAgentProposalStatus(i18n("Rejected — saved to TeX. PDF update failed."), KMessageWidget::Error, true);
        }
    });
    connect(m_fedoraRepository, &FedoraRepository::buildSourceDiffReady, this, &Part::fedoraBuildSourceDiffReady);
    connect(m_fedoraRepository, &FedoraRepository::buildInputSnapshotChanged, this, &Part::fedoraBuildInputSnapshotChanged);
    connect(m_fedoraRepository, &FedoraRepository::renderStateChanged, this, &Part::fedoraRenderStateChanged);
    // An HTML edition that was being built may be ready now (its state changes the render state).
    connect(m_fedoraRepository, &FedoraRepository::renderStateChanged, this, [this] {
        if (m_htmlReader) m_htmlReader->refreshLibraryState();
    });
    connect(m_fedoraRepository, &FedoraRepository::sourceConflictResolutionBundleReady, this, [this](const QJsonObject &bundle, const QString &error) {
        Q_EMIT sourceConflictResolutionBundleReady(bundle.toVariantMap(), error);
    });
    connect(m_fedoraRepository, &FedoraRepository::sourceConflictResolutionStatusChanged, this, &Part::sourceConflictResolutionStatusChanged);

    m_maxRecentItems = Okular::Settings::maxRecentItems();

    slotNewConfig();

    // keep us informed when the user changes settings
    connect(Okular::Settings::self(), &KCoreConfigSkeleton::configChanged, this, &Part::slotNewConfig);

#if HAVE_SPEECH
    // [SPEECH] check for TTS presence and usability
    Okular::Settings::setUseTTS(true);
    Okular::Settings::self()->save();
#endif

    rebuildBookmarkMenu(false);

    if (m_embedMode == ViewerWidgetMode) {
        // set the XML-UI resource file for the viewer mode
        setXMLFile(QStringLiteral("part-viewermode.rc"));
    } else {
        // set our main XML-UI resource file
        setXMLFile(QStringLiteral("part.rc"));
    }

    m_pageView->setupBaseActions(actionCollection());

    m_sidebar->setSidebarVisibility(false);
    if (m_embedMode != PrintPreviewMode) {
        // now set up actions that are required for all remaining modes
        m_pageView->setupViewerActions(actionCollection());
        // and if we are not in viewer mode, we want the full GUI
        if (m_embedMode != ViewerWidgetMode) {
            unsetDummyMode();
        }
    }

    // ensure history actions are in the correct state
    updateViewActions();

    // also update the state of the actions in the page view
    m_pageView->updateActionState(false, false);

    if (m_embedMode == NativeShellMode) {
        m_sidebar->setAutoFillBackground(false);
    }

#ifdef OKULAR_KEEP_FILE_OPEN
    m_keeper = new FileKeeper();
#endif
}

void Part::setupConfigSkeleton(const QVariantList &args)
{
    const QLatin1String configFileName("thoughtspartrc");

    // first, we check if a config file name has been specified
    QString configFilePath = detectConfigFileName(args);

    if (configFilePath.isEmpty()) {
        configFilePath = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + QLatin1Char('/') + configFileName;
    }

    KSharedConfigPtr config = KSharedConfig::openConfig(configFilePath);

    // Configuration update: SlidesTransitionsEnabled -> SlidesTransition = NoTransitions.
    // See https://invent.kde.org/graphics/okular/-/merge_requests/357
    KConfigGroup slidesConfigGroup = config.data()->group(QStringLiteral("Dlg Presentation"));
    if (slidesConfigGroup.readEntry<bool>("SlidesTransitionsEnabled", true) == false) {
        slidesConfigGroup.writeEntry("SlidesTransition", "NoTransitions");
    }
    slidesConfigGroup.deleteEntry("SlidesTransitionsEnabled");
    config.data()->sync();

    Okular::Settings::instance(config);
}

void Part::setupViewerActions()
{
    // ACTIONS
    KActionCollection *ac = actionCollection();

    // Page Traversal actions
    m_gotoPage = KStandardAction::gotoPage(this, SLOT(slotGoToPage()), ac);
    ac->setDefaultShortcuts(m_gotoPage, KStandardShortcut::gotoLine());
    // dirty way to activate gotopage when pressing miniBar's button
    connect(m_miniBar.data(), &MiniBar::gotoPage, m_gotoPage, &QAction::trigger);
    connect(m_pageNumberTool.data(), &MiniBar::gotoPage, m_gotoPage, &QAction::trigger);

    m_prevPage = KStandardAction::prior(this, SLOT(slotPreviousPage()), ac);
    m_prevPage->setIconText(i18nc("Previous page", "Previous"));
    m_prevPage->setToolTip(i18n("Go back to the Previous Page"));
    m_prevPage->setWhatsThis(i18n("Moves to the previous page of the document"));
    ac->setDefaultShortcut(m_prevPage, QKeySequence());
    // dirty way to activate prev page when pressing miniBar's button
    connect(m_miniBar.data(), &MiniBar::prevPage, m_prevPage, &QAction::trigger);
    connect(m_pageNumberTool.data(), &MiniBar::prevPage, m_prevPage, &QAction::trigger);

    m_nextPage = KStandardAction::next(this, SLOT(slotNextPage()), ac);
    m_nextPage->setIconText(i18nc("Next page", "Next"));
    m_nextPage->setToolTip(i18n("Advance to the Next Page"));
    m_nextPage->setWhatsThis(i18n("Moves to the next page of the document"));
    ac->setDefaultShortcut(m_nextPage, QKeySequence());
    // dirty way to activate next page when pressing miniBar's button
    connect(m_miniBar.data(), &MiniBar::nextPage, m_nextPage, &QAction::trigger);
    connect(m_pageNumberTool.data(), &MiniBar::nextPage, m_nextPage, &QAction::trigger);

    m_beginningOfDocument = KStandardAction::firstPage(this, SLOT(slotGotoFirst()), ac);
    ac->addAction(QStringLiteral("first_page"), m_beginningOfDocument);
    m_beginningOfDocument->setText(i18n("Beginning of the document"));
    m_beginningOfDocument->setWhatsThis(i18n("Moves to the beginning of the document"));

    m_endOfDocument = KStandardAction::lastPage(this, SLOT(slotGotoLast()), ac);
    ac->addAction(QStringLiteral("last_page"), m_endOfDocument);
    m_endOfDocument->setText(i18n("End of the document"));
    m_endOfDocument->setWhatsThis(i18n("Moves to the end of the document"));

    // we do not want back and next in history in the dummy mode
    m_historyBack = nullptr;
    m_historyNext = nullptr;

    m_addBookmark = KStandardAction::addBookmark(this, SLOT(slotAddBookmark()), ac);
    m_addBookmarkText = m_addBookmark->text();
    m_addBookmarkIcon = m_addBookmark->icon();
    m_bookmarkList->setAddBookmarkAction(m_addBookmark);

    m_renameBookmark = ac->addAction(QStringLiteral("rename_bookmark"));
    m_renameBookmark->setText(i18n("Rename Bookmark"));
    m_renameBookmark->setIcon(QIcon::fromTheme(QStringLiteral("edit-rename")));
    m_renameBookmark->setWhatsThis(i18n("Rename the current bookmark"));
    connect(m_renameBookmark, &QAction::triggered, this, &Part::slotRenameCurrentViewportBookmark);

    m_prevBookmark = ac->addAction(QStringLiteral("previous_bookmark"));
    m_prevBookmark->setText(i18n("Previous Bookmark"));
    m_prevBookmark->setIcon(QIcon::fromTheme(QStringLiteral("go-up-search")));
    m_prevBookmark->setWhatsThis(i18n("Go to the previous bookmark"));
    connect(m_prevBookmark, &QAction::triggered, this, &Part::slotPreviousBookmark);

    m_nextBookmark = ac->addAction(QStringLiteral("next_bookmark"));
    m_nextBookmark->setText(i18n("Next Bookmark"));
    m_nextBookmark->setIcon(QIcon::fromTheme(QStringLiteral("go-down-search")));
    m_nextBookmark->setWhatsThis(i18n("Go to the next bookmark"));
    connect(m_nextBookmark, &QAction::triggered, this, &Part::slotNextBookmark);

    m_copy = nullptr;
    m_copyWithoutLineBreaks = nullptr;

    m_selectAll = nullptr;
    m_selectCurrentPage = nullptr;

    // Find and other actions
    m_find = KStandardAction::find(this, SLOT(slotShowFindBar()), ac);
    QList<QKeySequence> s = m_find->shortcuts();
    s.append(QKeySequence(Qt::Key_Slash));
    ac->setDefaultShortcuts(m_find, s);
    m_find->setEnabled(false);

    m_findNext = KStandardAction::findNext(this, SLOT(slotFindNext()), ac);
    m_findNext->setEnabled(false);

    m_findPrev = KStandardAction::findPrev(this, SLOT(slotFindPrev()), ac);
    m_findPrev->setEnabled(false);

    m_openContainingFolder = nullptr;

    m_hamburgerMenuAction = nullptr;

    QAction *prefs = KStandardAction::preferences(this, SLOT(slotPreferences()), ac);
    if (m_embedMode == NativeShellMode) {
        prefs->setText(i18n("Configure Thoughts…"));
    } else {
        // TODO: improve this message
        prefs->setText(i18n("Configure Viewer…"));
    }

    m_printPreview = KStandardAction::printPreview(this, SLOT(slotPrintPreview()), ac);
    m_printPreview->setEnabled(false);

    m_showLeftPanel = nullptr;
    m_showBottomBar = nullptr;

    m_showProperties = ac->addAction(QStringLiteral("properties"));
    m_showProperties->setText(i18n("&Properties"));
    m_showProperties->setIcon(QIcon::fromTheme(QStringLiteral("document-properties")));
    ac->setDefaultShortcuts(m_showProperties, {QKeySequence(Qt::ALT | Qt::Key_Return)});
    connect(m_showProperties, &QAction::triggered, this, &Part::slotShowProperties);
    m_showProperties->setEnabled(false);

    m_showEmbeddedFiles = nullptr;
    m_showPresentation = nullptr;

    m_exportAs = nullptr;
    m_exportAsMenu = nullptr;
    m_exportAsText = nullptr;
    m_exportAsPdfWithComments = nullptr;

#if HAVE_PURPOSE
    m_share = nullptr;
    m_shareMenu = nullptr;
#endif

    m_presentationDrawingActions = nullptr;

    m_aboutBackend = ac->addAction(QStringLiteral("help_about_backend"));
    m_aboutBackend->setText(i18n("About Backend"));
    m_aboutBackend->setEnabled(false);
    connect(m_aboutBackend, &QAction::triggered, this, &Part::slotAboutBackend);

    QAction *reloadAction = ac->add<QAction>(QStringLiteral("file_reload"));
    reloadAction->setText(i18n("Reloa&d"));
    reloadAction->setIcon(QIcon::fromTheme(QStringLiteral("view-refresh")));
    reloadAction->setWhatsThis(i18n("Reload the current document from disk."));
    connect(reloadAction, &QAction::triggered, this, &Part::slotReload);
    ac->setDefaultShortcut(reloadAction, QKeySequence(Qt::ALT | Qt::Key_F5));
    m_reload = reloadAction;

    QAction *updateAction = ac->addAction(QStringLiteral("update_from_fedora"));
    updateAction->setText(i18n("Sync"));
    updateAction->setIcon(QIcon::fromTheme(QStringLiteral("view-refresh")));
    updateAction->setToolTip(i18n("Publish local sources and lightweight changes, then fetch and activate the latest server state"));
    connect(updateAction, &QAction::triggered, this, [this] {
        if (!url().isLocalFile()) {
            return;
        }
        Q_EMIT globalSyncRequested();
    });
    const auto updateManualSyncAction = [this, updateAction](FedoraRepository::ManualSyncState state, const QString &message, const QString &details) {
        Q_UNUSED(message)
        Q_UNUSED(details)
        const bool running = state == FedoraRepository::ManualSyncState::Running;
        updateAction->setEnabled(!running);
        updateAction->setText(running ? Thoughts::SyncStatusLabels::syncing() : i18n("Sync"));
        if (!running && m_manualSyncRequestedByPart) {
            if (state == FedoraRepository::ManualSyncState::Succeeded || state == FedoraRepository::ManualSyncState::Failed) {
                m_manualSyncRequestedByPart = false;
            }
        }
    };
    connect(m_fedoraRepository, &FedoraRepository::manualSyncStateChanged, this, updateManualSyncAction);
    updateManualSyncAction(m_fedoraRepository->manualSyncState(), m_fedoraRepository->manualSyncMessage(), m_fedoraRepository->manualSyncDetails());

    m_closeFindBar = ac->addAction(QStringLiteral("close_find_bar"), this, SLOT(slotHideFindBar()));
    m_closeFindBar->setText(i18n("Close &Search"));
    ac->setDefaultShortcut(m_closeFindBar, QKeySequence(Qt::Key_Escape));
    m_closeFindBar->setEnabled(false);

    QWidgetAction *pageno = new QWidgetAction(ac);
    pageno->setText(i18n("Page Number"));
    pageno->setDefaultWidget(m_pageNumberTool);
    ac->addAction(QStringLiteral("page_number"), pageno);

    QAction *configureColorModes = new QAction(i18nc("@action", "Configure Color Modes…"), ac);
    ac->addAction(QStringLiteral("options_configure_color_modes"), configureColorModes);
    configureColorModes->setIcon(QIcon::fromTheme(QStringLiteral("configure")));
    connect(configureColorModes, &QAction::triggered, this, &Part::slotAccessibilityPreferences);
}

void Part::setViewerShortcuts()
{
    KActionCollection *ac = actionCollection();

    ac->setDefaultShortcut(m_gotoPage, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_G));
    ac->setDefaultShortcut(m_find, QKeySequence());

    ac->setDefaultShortcut(m_findNext, QKeySequence());
    ac->setDefaultShortcut(m_findPrev, QKeySequence());

    ac->setDefaultShortcut(m_addBookmark, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_B));

    ac->setDefaultShortcut(m_beginningOfDocument, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Home));
    ac->setDefaultShortcut(m_endOfDocument, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_End));

    QAction *action = static_cast<QAction *>(ac->action(QStringLiteral("file_reload")));
    if (action) {
        ac->setDefaultShortcut(action, QKeySequence(Qt::ALT | Qt::Key_F5));
    }
}

void Part::setupActions()
{
    KActionCollection *ac = actionCollection();

    auto manager = new KColorSchemeManager(this);
    KActionMenu *schemeMenu = KColorSchemeMenu::createMenu(manager, this);
    ac->addAction(QStringLiteral("colorscheme_menu"), schemeMenu->menu()->menuAction());

    m_copy = KStandardAction::create(KStandardAction::Copy, nullptr, nullptr, ac);
    connect(m_copy, &QAction::triggered, this, &Part::slotCopyTextSelectionOrAnnotation);

    // Copy-without-line-breaks action
    m_copyWithoutLineBreaks = ac->addAction(QStringLiteral("edit_copy_without_line_breaks"));
    m_copyWithoutLineBreaks->setText(i18n("Copy Text (Without line breaks)"));
    ac->setDefaultShortcut(m_copyWithoutLineBreaks, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    m_copyWithoutLineBreaks->setIcon(QIcon::fromTheme(QStringLiteral("edit-copy")));
    connect(m_copyWithoutLineBreaks, &QAction::triggered, this, [this]() { m_pageView->copyTextSelection(PageView::TextCopyMode::WithoutLineBreaks); });
    m_copyWithoutLineBreaks->setEnabled(false);

    m_selectAll = KStandardAction::selectAll(m_pageView, SLOT(selectAll()), ac);

    // Setup select all action for the current page
    m_selectCurrentPage = ac->addAction(QStringLiteral("edit_select_all_current_page"));
    m_selectCurrentPage->setText(i18n("Select All Text on Current Page"));
    connect(m_selectCurrentPage, &QAction::triggered, m_pageView, &PageView::slotSelectPage);
    m_selectCurrentPage->setEnabled(false);

    m_showLeftPanel = ac->add<KToggleAction>(QStringLiteral("show_leftpanel"));
    m_showLeftPanel->setText(i18n("Show S&idebar"));
    const QString preferredSidebarIcon = m_sidebar->layoutDirection() == Qt::LeftToRight ? QStringLiteral("sidebar-expand-left") : QStringLiteral("sidebar-expand-right");
    m_showLeftPanel->setIcon(QIcon::fromTheme(preferredSidebarIcon, QIcon::fromTheme(QStringLiteral("view-sidetree"))));
    connect(m_showLeftPanel, &QAction::toggled, this, &Part::slotShowLeftPanel);
    ac->setDefaultShortcut(m_showLeftPanel, QKeySequence(Qt::Key_F7));
    m_showLeftPanel->setChecked(Okular::Settings::showLeftPanel());
    slotShowLeftPanel();

    // The toolbar's "Sidebar" chooser replaces the sidebar's tab strip. Its
    // checked entry always follows show_leftpanel and the current panel,
    // however they change (F7, the toggle icon, restored window state, ...).
    m_sidebarDisplay = new KActionMenu(QIcon::fromTheme(QStringLiteral("view-sidetree"), QIcon::fromTheme(QStringLiteral("view-split-left-right"))), i18n("Sidebar"), this);
    m_sidebarDisplay->setPopupMode(QToolButton::InstantPopup);
    m_sidebarDisplay->setToolTip(i18n("Choose sidebar display"));
    ac->addAction(QStringLiteral("sidebar_display"), m_sidebarDisplay);
    auto *sidebarDisplayGroup = new QActionGroup(m_sidebarDisplay);
    sidebarDisplayGroup->setExclusive(true);
    const auto addSidebarDisplay = [this, ac, sidebarDisplayGroup](const QString &name, const QString &text, const std::function<QWidget *()> &item) {
        QAction *action = ac->addAction(name);
        action->setText(text);
        action->setCheckable(true);
        sidebarDisplayGroup->addAction(action);
        m_sidebarDisplay->addAction(action);
        connect(action, &QAction::triggered, this, [this, item] { chooseSidebarDisplay(item()); });
        return action;
    };
    m_sidebarHideAction = addSidebarDisplay(QStringLiteral("sidebar_display_hide"), i18n("Hide sidebar"), [] { return nullptr; });
    m_sidebarAnnotationsAction = addSidebarDisplay(QStringLiteral("sidebar_display_annotations"), i18n("Annotations"), [this] { return static_cast<QWidget *>(m_reviewsWidget.data()); });
    m_sidebarContentsAction = addSidebarDisplay(QStringLiteral("sidebar_display_contents"), i18n("Contents"), [this] { return static_cast<QWidget *>(m_toc.data()); });
    m_sidebarContentsAction->setToolTip(i18n("Contents of the document (only for documents with an outline)"));
    // The Shell gives a Library document its History (setHistorySidebarPanel).
    m_sidebarHistoryAction = addSidebarDisplay(QStringLiteral("sidebar_display_history"), i18n("History"), [this] { return m_historyPanel.data(); });
    m_sidebarHistoryAction->setToolTip(i18n("Every version of the document, with its review cards and their feedback"));
    m_sidebarHistoryAction->setVisible(false);
    // The Shell gives every document the agents' notes (setAgentNotesSidebarPanel).
    m_sidebarAgentNotesAction = addSidebarDisplay(QStringLiteral("sidebar_display_agent_notes"), i18n("Agent Notes"), [this] { return m_agentNotesPanel.data(); });
    m_sidebarAgentNotesAction->setToolTip(i18n("Notes agents sent to your phone, with your replies"));
    m_sidebarAgentNotesAction->setVisible(false);
    connect(m_sidebar, &Sidebar::currentItemChanged, this, &Part::updateSidebarDisplayActions);
    updateSidebarDisplayActions();

    QAction *overviewAction = ac->addAction(QStringLiteral("comments_overview"));
    overviewAction->setText(i18n("Thoughts Overview…"));
    overviewAction->setIconText(i18n("Overview"));
    overviewAction->setIcon(QIcon::fromTheme(QStringLiteral("view-list-details")));
    overviewAction->setToolTip(i18n("Show the Thoughts Overview across tracked documents"));
    overviewAction->setWhatsThis(i18n("Open the Thoughts Overview with its summary, comments, and review cards."));
    overviewAction->setPriority(QAction::NormalPriority);
    connect(overviewAction, &QAction::triggered, this, &Part::requestCommentsOverview);
    QAction *logsAction = ac->addAction(QStringLiteral("sync_operations"));
    logsAction->setText(i18n("Logs…"));
    logsAction->setIcon(QIcon::fromTheme(QStringLiteral("view-history")));
    logsAction->setToolTip(i18n("Show previous synchronization and build logs"));
    connect(logsAction, &QAction::triggered, this, &Part::requestSyncOperations);

    m_showBottomBar = ac->add<KToggleAction>(QStringLiteral("show_bottombar"));
    m_showBottomBar->setText(i18n("Show &Page Bar"));
    connect(m_showBottomBar, &QAction::toggled, this, &Part::slotShowBottomBar);
    m_showBottomBar->setChecked(Okular::Settings::showBottomBar());
    slotShowBottomBar();

    m_showEmbeddedFiles = ac->addAction(QStringLiteral("embedded_files"));
    m_showEmbeddedFiles->setText(i18n("&Embedded Files"));
    m_showEmbeddedFiles->setIcon(QIcon::fromTheme(QStringLiteral("mail-attachment")));
    connect(m_showEmbeddedFiles, &QAction::triggered, this, &Part::slotShowEmbeddedFiles);
    m_showEmbeddedFiles->setEnabled(false);

    m_exportAs = ac->addAction(QStringLiteral("file_export_as"));
    m_exportAs->setText(i18n("E&xport As"));
    m_exportAs->setIcon(QIcon::fromTheme(QStringLiteral("document-export")));
    m_exportAsMenu = new QMenu();
    connect(m_exportAsMenu, &QMenu::triggered, this, [this](QAction *action) {
        if (action != m_exportAsPdfWithComments) {
            slotExportAs(action);
        }
    });
    m_exportAs->setMenu(m_exportAsMenu);
    m_exportAsPdfWithComments = ac->addAction(QStringLiteral("file_export_pdf_with_comments"));
    m_exportAsPdfWithComments->setText(i18n("PDF with Comments…"));
    m_exportAsPdfWithComments->setIcon(QIcon::fromTheme(QStringLiteral("application-pdf")));
    m_exportAsPdfWithComments->setEnabled(false);
    m_exportAsMenu->addAction(m_exportAsPdfWithComments);
    connect(m_exportAsPdfWithComments, &QAction::triggered, this, [this] { slotExportAs(m_exportAsPdfWithComments); });
    m_exportAsText = actionForExportFormat(Okular::ExportFormat::standardFormat(Okular::ExportFormat::PlainText), m_exportAsMenu);
    m_exportAsMenu->addAction(m_exportAsText);
    m_exportAs->setEnabled(false);
    m_exportAsText->setEnabled(false);

#if HAVE_PURPOSE
    m_share = ac->addAction(QStringLiteral("file_share"));
    m_share->setText(i18n("S&hare"));
    m_share->setIcon(QIcon::fromTheme(QStringLiteral("document-share")));
    m_share->setEnabled(false);
    m_shareMenu = new Purpose::Menu();
    connect(m_shareMenu, &Purpose::Menu::finished, this, &Part::slotShareActionFinished);
    m_share->setMenu(m_shareMenu);
#endif

    m_showPresentation = ac->addAction(QStringLiteral("presentation"));
    m_showPresentation->setText(i18n("P&resentation"));
    m_showPresentation->setIcon(QIcon::fromTheme(QStringLiteral("view-presentation")));
    connect(m_showPresentation, &QAction::triggered, this, &Part::slotShowPresentation);
    ac->setDefaultShortcut(m_showPresentation, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P));
    m_showPresentation->setEnabled(false);

    m_openContainingFolder = ac->addAction(QStringLiteral("open_containing_folder"));
    m_openContainingFolder->setText(i18n("Open Con&taining Folder"));
    m_openContainingFolder->setIcon(QIcon::fromTheme(QStringLiteral("document-open-folder")));
    connect(m_openContainingFolder, &QAction::triggered, this, &Part::slotOpenContainingFolder);
    m_openContainingFolder->setEnabled(false);

    if (m_embedMode == Okular::NativeShellMode) { // This hamburger menu is designed to be quite Okular-specific.
        m_hamburgerMenuAction = KStandardAction::hamburgerMenu(nullptr, nullptr, ac);
        if (auto *mainWindow = findMainWindow()) {
            m_hamburgerMenuAction->setMenuBar(mainWindow->menuBar());
        }
        connect(m_hamburgerMenuAction, &KHamburgerMenu::aboutToShowMenu, this, &Part::slotUpdateHamburgerMenu);
    }

    QAction *importPS = ac->addAction(QStringLiteral("import_ps"));
    importPS->setText(i18n("&Import PostScript as PDF…"));
    importPS->setIcon(QIcon::fromTheme(QStringLiteral("document-import")));
    connect(importPS, &QAction::triggered, this, &Part::slotImportPSFile);

    KToggleAction *blackscreenAction = new KToggleAction(i18n("Switch Blackscreen Mode"), ac);
    ac->addAction(QStringLiteral("switch_blackscreen_mode"), blackscreenAction);
    ac->setDefaultShortcut(blackscreenAction, QKeySequence(Qt::Key_B));
    blackscreenAction->setIcon(QIcon::fromTheme(QStringLiteral("view-presentation")));
    blackscreenAction->setEnabled(false);

    m_presentationDrawingActions = new DrawingToolActions(ac);

    QAction *eraseDrawingAction = new QAction(i18n("Erase Drawing"), ac);
    ac->addAction(QStringLiteral("presentation_erase_drawings"), eraseDrawingAction);
    eraseDrawingAction->setIcon(QIcon::fromTheme(QStringLiteral("draw-eraser-delete-objects")));
    eraseDrawingAction->setEnabled(false);

    QAction *configureAnnotations = new QAction(i18n("Configure Annotations…"), ac);
    ac->addAction(QStringLiteral("options_configure_annotations"), configureAnnotations);
    configureAnnotations->setIcon(QIcon::fromTheme(QStringLiteral("configure")));
    connect(configureAnnotations, &QAction::triggered, this, &Part::slotAnnotationPreferences);

    QAction *playPauseAction = new QAction(i18n("Play/Pause Presentation"), ac);
    ac->addAction(QStringLiteral("presentation_play_pause"), playPauseAction);
    playPauseAction->setEnabled(false);
}

Part::~Part()
{
#if HAVE_DBUS
    QDBusConnection::sessionBus().unregisterObject(m_registerDbusName);
#endif // HAVE_DBUS

    m_document->removeObserver(this);

    if (m_document->isOpened()) {
        Part::closeUrl(false);
    }

    delete m_toc;
    delete m_layers;
    delete m_pageView;
    delete m_thumbnailList;
    delete m_miniBar;
    delete m_pageNumberTool;
    delete m_miniBarLogic;
    delete m_bottomBar;
    delete m_pageSizeLabel;
    delete m_reviewsWidget;
    delete m_bookmarkList;
    delete m_infoTimer;
    delete m_signaturePanel;

    delete m_document;

    delete m_tempfile;

    qDeleteAll(m_bookmarkActions);

    delete m_exportAsMenu;
#if HAVE_PURPOSE
    delete m_shareMenu;
#endif

#ifdef OKULAR_KEEP_FILE_OPEN
    delete m_keeper;
#endif
}

bool Part::openDocument(const QUrl &url, uint page)
{
    Okular::DocumentViewport vp(page - 1);
    vp.rePos.enabled = true;
    vp.rePos.normalizedX = 0;
    vp.rePos.normalizedY = 0;
    vp.rePos.pos = Okular::DocumentViewport::TopLeft;
    if (vp.isValid()) {
        m_document->setNextDocumentViewport(vp);
    }
    return openUrl(url);
}

void Part::startPresentation()
{
    m_cliPresentation = true;
}

QStringList Part::supportedMimeTypes() const
{
    return m_document->supportedMimeTypes();
}

QUrl Part::realUrl() const
{
    if (!m_realUrl.isEmpty()) {
        return m_realUrl;
    }

    return url();
}

// ViewerInterface

void Part::showSourceLocation(const QString &fileName, int line, int column, bool showGraphically)
{
    Q_UNUSED(column);

    const QString u = QStringLiteral("src:%1 %2").arg(line + 1).arg(fileName);
    GotoAction action(QString(), u);
    m_document->processAction(&action);
    if (showGraphically) {
        m_pageView->setLastSourceLocationViewport(m_document->viewport());
    }
}

void Part::clearLastShownSourceLocation()
{
    m_pageView->clearLastSourceLocationViewport();
}

bool Part::isWatchFileModeEnabled() const
{
    return !m_watcher->signalsBlocked();
}

void Part::setWatchFileModeEnabled(bool enabled)
{
    // Don't call 'KDirWatch::stopScan()' in here (as of KDE Frameworks 5.51.0, see bug 400541)!
    // 'KDirWatch::stopScan' has a bug that may affect other code paths that make use of KDirWatch
    // (other loaded KParts, for example).
    if (isWatchFileModeEnabled() == enabled) {
        return;
    }

    m_watcher->blockSignals(!enabled);

    if (!enabled) {
        m_dirtyHandler->stop();
    }
}

bool Part::areSourceLocationsShownGraphically() const
{
    return m_pageView->areSourceLocationsShownGraphically();
}

void Part::setShowSourceLocationsGraphically(bool show)
{
    m_pageView->setShowSourceLocationsGraphically(show);
}

bool Part::openNewFilesInTabs() const
{
    return Okular::Settings::self()->shellOpenFileInTabs();
}

QWidget *Part::getSideContainer() const
{
    return m_sidebar->getSideContainer();
}

bool Part::activateTabIfAlreadyOpenFile() const
{
    return Okular::Settings::self()->switchToTabIfOpen();
}

void Part::setModified(bool modified)
{
    KParts::ReadWritePart::setModified(modified);
    if (!modified) {
        if (m_docsCommentRefreshQueued) {
            scheduleDocsCommentRefresh();
        }
        if (m_docsSourceReconcileQueued) {
            scheduleDocsSourceReconciliation();
        }
    }
}

void Part::makeExistingAnnotationsReadOnly()
{
    for (uint pageNumber = 0; pageNumber < m_document->pages(); ++pageNumber) {
        const Okular::Page *page = m_document->page(pageNumber);
        if (!page) {
            continue;
        }
        for (Okular::Annotation *annotation : page->annotations()) {
            // The PDF's own annotations; comments kept on this computer stay editable.
            if (annotation && (annotation->flags() & Okular::Annotation::External)) {
                annotation->setFlags(annotation->flags() | Okular::Annotation::DenyWrite | Okular::Annotation::DenyDelete);
            }
        }
    }
}

void Part::slotHandleActivatedSourceReference(const QString &absFileName, int line, int col, bool *handled)
{
    Q_EMIT openSourceReference(absFileName, line, col);
    if (m_embedMode == Okular::ViewerWidgetMode) {
        *handled = true;
    }
}

DocsCommentSynchronizer::SourceResolutionResult Part::resolveTexSource(int pageNumber, double normalizedX, double normalizedY, const QString &selectedText)
{
    const QUrl documentUrl = m_document->currentDocument();
    const int pageIndex = pageNumber - 1;
    const Okular::Page *page = pageIndex >= 0 ? m_document->page(pageIndex) : nullptr;
    if (!documentUrl.isLocalFile() || !page || !std::isfinite(normalizedX) || !std::isfinite(normalizedY) || normalizedX < 0.0 || normalizedX > 1.0 || normalizedY < 0.0 || normalizedY > 1.0) {
        DocsCommentSynchronizer::SourceResolutionResult invalid;
        invalid.failureReason = i18n("The selected location could not be mapped to a TeX source location.");
        return invalid;
    }

    const QSizeF documentDpi = m_docsDocumentDpi.isValid() ? m_docsDocumentDpi : QSizeF(72.0, 72.0);
    const double x = normalizedX * page->width() * 72.0 / documentDpi.width();
    const double y = normalizedY * page->height() * 72.0 / documentDpi.height();
    DocsCommentSynchronizer synchronizer({
        .docsRoot = m_docsRenderRoot,
        .sourceBundleRoot = QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server/source-bundle")),
        .synctexExecutable = m_docsSyncTexExecutable,
        .processEnvironment = docsToolEnvironment(),
        .synctexTimeoutMs = m_docsSyncTexTimeoutMs,
        .lockTimeoutMs = 2000,
        .allowLegacySourceResolution = false,
    });
    const QFileInfo pdfInfo(documentUrl.toLocalFile());
    const auto reading = Thoughts::ProjectReadingContext::load(pdfInfo.absoluteFilePath());
    const auto *child = reading ? reading->locate(pageIndex) : nullptr;
    DocsCommentSynchronizer::SourceResolutionResult result = synchronizer.resolveSource({
        .pdfStem = child ? child->stem() : pdfInfo.completeBaseName(),
        .pageNumber = child ? pageNumber - child->document.firstPage : pageNumber,
        .x = x,
        .y = y,
        .selectedText = selectedText,
        .pdfPath = child ? child->path : pdfInfo.absoluteFilePath(),
    });
    if (result.success || result.problem != DocsCommentSynchronizer::SourceResolutionResult::Problem::Unavailable) {
        return result;
    }

    std::unique_ptr<const Okular::SourceReference> fallback(m_document->dynamicSourceReference(pageIndex, normalizedX * page->width(), normalizedY * page->height()));
    if (fallback && fallback->row() > 0) {
        const QFileInfo sourceInfo(fallback->fileName());
        const QString sourcePath = sourceInfo.isAbsolute() ? sourceInfo.absoluteFilePath() : QDir(pdfInfo.absolutePath()).absoluteFilePath(fallback->fileName());
        const QFileInfo absoluteSource(sourcePath);
        if (absoluteSource.isFile()) {
            const QString canonicalPath = absoluteSource.canonicalFilePath().isEmpty() ? absoluteSource.absoluteFilePath() : absoluteSource.canonicalFilePath();
            result.success = true;
            result.sourcePath = canonicalPath;
            result.line = fallback->row();
            return result;
        }
    }

    return result;
}

void Part::openTexSource(int pageNumber, double normalizedX, double normalizedY, const QString &selectedText)
{
    const auto result = resolveTexSource(pageNumber, normalizedX, normalizedY, selectedText);
    if (result.success) {
        Q_EMIT requestOpenSourceSelection(result.sourcePath, result.line, selectedText);
    } else if (result.problem != DocsCommentSynchronizer::SourceResolutionResult::Problem::Unavailable) {
        showSourceLocationFallback(result.fallbackSourcePath,
                                   result.problem == DocsCommentSynchronizer::SourceResolutionResult::Problem::GeneratedContent);
    } else {
        KMessageBox::error(widget(),
                       i18n("The selected location could not be mapped to a TeX source location.\n\n%1", result.failureReason),
                       i18n("Open source"));
    }
}

void Part::showSourceLocationFallback(const QString &sourcePath, bool generated)
{
    const QFileInfo source(sourcePath);
    const bool canOpen = !sourcePath.isEmpty() && source.isFile() && source.isReadable();
    const QString message = generated
        ? i18n("This content is generated automatically.")
        : i18n("Thoughts found the source file, but could not reliably locate this selection in its current version.");
    QMessageBox dialog(QMessageBox::Information, i18n("Open source"), message, QMessageBox::Close, widget());
    dialog.setObjectName(QStringLiteral("sourceLocationFallbackDialog"));
    dialog.setTextFormat(Qt::PlainText);
    QPushButton *openButton = nullptr;
    if (canOpen) {
        dialog.setInformativeText(generated
            ? i18n("You can open the document source that includes it.\n\n%1", QDir::toNativeSeparators(source.canonicalFilePath()))
            : i18n("You can open the source file without jumping to a specific line.\n\n%1", QDir::toNativeSeparators(source.canonicalFilePath())));
        openButton = dialog.addButton(generated ? i18n("Open document source") : i18n("Open source file"), QMessageBox::AcceptRole);
        dialog.setDefaultButton(openButton);
    } else {
        dialog.setInformativeText(generated
            ? i18n("Thoughts could not identify a single available document source to open.")
            : i18n("The source file is no longer available at its recorded location."));
    }
    dialog.exec();
    if (openButton && dialog.clickedButton() == openButton) {
        // Zero means file-only navigation, not an estimated source line.
        Q_EMIT requestOpenCommentSource(source.canonicalFilePath(), 0);
    }
}

void Part::openUrlFromDocument(const QUrl &url)
{
    if (m_embedMode == PrintPreviewMode) {
        return;
    }

    if (url.isLocalFile()) {
        if (!QFile::exists(url.toLocalFile())) {
            KMessageBox::error(widget(), i18n("Could not open '%1'. File does not exist", url.toDisplayString()));
            return;
        }
    } else {
        KIO::StatJob *statJob = KIO::stat(url, KIO::StatJob::SourceSide);
        KJobWidgets::setWindow(statJob, widget());
        if (!statJob->exec() || statJob->error()) {
            KMessageBox::error(widget(), i18n("Could not open '%1' (%2) ", url.toDisplayString(), statJob->errorString()));
            return;
        }
    }

    Q_EMIT m_bExtension->openUrlNotify();
    Q_EMIT m_bExtension->setLocationBarUrl(url.toDisplayString());
    openUrl(url);
}

void Part::openUrlFromBookmarks(const QUrl &_url)
{
    QUrl url = _url;
    Okular::DocumentViewport vp(_url.fragment(QUrl::FullyDecoded));
    if (vp.isValid()) {
        m_document->setNextDocumentViewport(vp);
    }
    url.setFragment(QString());
    if (m_document->currentDocument() == url) {
        if (vp.isValid()) {
            m_document->setViewport(vp);
        }
    } else {
        openUrl(url);
    }
}

void Part::handleDroppedUrls(const QList<QUrl> &urls)
{
    if (urls.isEmpty()) {
        return;
    }

    if (m_embedMode != NativeShellMode || !openNewFilesInTabs()) {
        openUrlFromDocument(urls.first());
        return;
    }

    Q_EMIT urlsDropped(urls);
}

void Part::slotJobStarted(KIO::Job *job)
{
    if (job) {
        job->addMetaData(QStringLiteral("accept"), supportedMimeTypes().join(QStringLiteral(", ")) + QStringLiteral(", */*;q=0.5"));

        connect(job, &KJob::result, this, &Part::slotJobFinished);
    }
}

void Part::slotJobFinished(KJob *job)
{
    if (job->error() == KIO::ERR_USER_CANCELED) {
        m_pageView->displayMessage(i18n("The loading of %1 has been canceled.", realUrl().toDisplayString(QUrl::PreferLocalFile)));
    }
}

void Part::loadCancelled(const QString &reason)
{
    Q_EMIT setWindowCaption(QString());
    resetStartArguments();

    // when m_viewportDirty.pageNumber != -1 we come from slotAttemptReload
    // so we don't want to show an ugly messagebox just because the document is
    // taking more than usual to be recreated
    if (m_viewportDirty.pageNumber == -1) {
        if (m_urlWithFragment.isValid() && !m_urlWithFragment.isLocalFile()) {
            tryOpeningUrlWithFragmentAsName();
        } else if (!reason.isEmpty()) {
            KMessageBox::error(widget(), i18n("Could not open %1. Reason: %2", url().toDisplayString(), reason));
        }
    }
}

void Part::setWindowTitleFromDocument()
{
    // If 'DocumentTitle' should be used, check if the document has one. If
    // either case is false, use the file name.
    QString title = Okular::Settings::displayDocumentNameOrPath() == Okular::Settings::EnumDisplayDocumentNameOrPath::Path ? realUrl().toDisplayString(QUrl::PreferLocalFile) : realUrl().fileName();

    if (Okular::Settings::displayDocumentTitle()) {
        const QString docTitle = m_document->metaData(QStringLiteral("DocumentTitle")).toString();
        if (!docTitle.isEmpty() && !docTitle.trimmed().isEmpty()) {
            title = docTitle;
        }
    }

    Q_EMIT setWindowCaption(title);
}

void Part::notifySetup(const QList<Okular::Page *> & /*pages*/, int setupFlags)
{
    // Hide the migration message if the user has just migrated. Otherwise,
    // if m_migrationMessage is already hidden, this does nothing.
    if (!m_document->isDocdataMigrationNeeded()) {
        m_migrationMessage->animatedHide();
    }

    if (!(setupFlags & Okular::DocumentObserver::DocumentChanged)) {
        return;
    }

    rebuildBookmarkMenu();
    updateAboutBackendAction();
    if (m_sidebar->isTransientWidgetVisible()) {
        slotHideFindBar();
    } else {
        m_findBar->resetSearch();
    }
}

void Part::notifyViewportChanged(bool /*smoothMove*/)
{
    updateViewActions();
    if (!m_docsOpenedPdfIdentityVerified) return;
    if (const auto reading = Thoughts::ProjectReadingContext::load(m_docsOpenedPdfPath)) {
        const int page = m_document->viewport().pageNumber;
        if (const auto *child = reading->locate(page)) {
            QSettings position;
            const auto key = QStringLiteral("project-reading/") + QString::fromLatin1(QCryptographicHash::hash(reading->reading.id.toUtf8(), QCryptographicHash::Sha256).toHex());
            position.setValue(key + QStringLiteral("/document"), child->document.id);
            position.setValue(key + QStringLiteral("/page"), page - child->document.firstPage);
        }
    }
}

void Part::notifyPageChanged(int page, int flags)
{
    if (!(flags & Okular::DocumentObserver::Bookmark)) {
        return;
    }

    rebuildBookmarkMenu();
    if (page == m_document->viewport().pageNumber) {
        updateBookmarksActions();
    }
}

void Part::goToPage(uint page)
{
    if (page <= m_document->pages()) {
        m_document->setViewportPage(page - 1);
    }
}

void Part::openDocument(const QString &doc)
{
    openUrl(QUrl::fromUserInput(doc));
}

uint Part::pages()
{
    return m_document->pages();
}

QString Part::selectedText() const
{
    // An HTML attachment or the HTML view: the page's last reported selection (never masked text).
    if (m_attachmentShown && m_attachmentReader) return m_attachmentReader->lastSelection().value(QStringLiteral("text")).toString();
    if (htmlViewActive()) return m_htmlReader->lastSelection().value(QStringLiteral("text")).toString();
    return m_pageView ? m_pageView->selectedText() : QString();
}

uint Part::currentPage()
{
    return m_document->pages() ? m_document->currentPage() + 1 : 0;
}

QString Part::currentDocument()
{
    return m_document->currentDocument().toDisplayString(QUrl::PreferLocalFile);
}

QString Part::documentMetaData(const QString &metaData) const
{
    const Okular::DocumentInfo info = m_document->documentInfo();
    return info.get(metaData);
}

bool Part::slotImportPSFile()
{
    QString app = QStandardPaths::findExecutable(QStringLiteral("ps2pdf"));
    if (app.isEmpty()) {
        // TODO point the user to their distro packages?
        KMessageBox::error(widget(), i18n("The program \"ps2pdf\" was not found, so Thoughts can not import PS files using it."), i18n("ps2pdf not found"));
        return false;
    }

    QMimeDatabase mimeDatabase;
    QString filter = i18n("PostScript files (%1)", mimeDatabase.mimeTypeForName(QStringLiteral("application/postscript")).globPatterns().join(QLatin1Char(' ')));

    QUrl url = QFileDialog::getOpenFileUrl(widget(), QString(), QUrl(), filter);
    if (url.isLocalFile()) {
        QTemporaryFile tf(QDir::tempPath() + QLatin1String("/okular_XXXXXX.pdf"));
        tf.setAutoRemove(false);
        if (!tf.open()) {
            return false;
        }
        m_temporaryLocalFile = tf.fileName();
        tf.close();

        setLocalFilePath(url.toLocalFile());
        QStringList args;
        QProcess *p = new QProcess();
        args << url.toLocalFile() << m_temporaryLocalFile;
        m_pageView->displayMessage(i18n("Importing PS file as PDF (this may take a while)…"));
        connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &Part::psTransformEnded);
        p->start(app, args);
        return true;
    }

    m_temporaryLocalFile.clear();
    return false;
}

void Part::setFileToWatch(const QString &filePath)
{
    if (!m_watchedFilePath.isEmpty()) {
        unsetFileToWatch();
    }

    const QFileInfo fi(filePath);

    m_watchedFilePath = filePath;
    m_watcher->addFile(m_watchedFilePath);

    if (fi.isSymLink()) {
        m_watchedFileSymlinkTarget = fi.symLinkTarget();
        m_watcher->addFile(m_watchedFileSymlinkTarget);
    } else {
        m_watchedFileSymlinkTarget.clear();
    }
    const QUrl documentUrl = QUrl::fromLocalFile(filePath);
    if (shouldSyncDocsComments(documentUrl)) {
        const QString pdfPath = QFileInfo(filePath).absoluteFilePath();
        if (m_docsCommentPdfPath != pdfPath) {
            clearDocsCommentWatcher();
            m_docsCommentPdfPath = pdfPath;
        }
    } else {
        clearDocsCommentWatcher();
    }
    // Accessing source/comment companions next to a document can trigger a
    // platform permission or file-provider round trip. Document opening runs
    // before the application event loop, so doing that work here can leave the
    // whole window stuck behind an invisible permission request. Let the first
    // event-loop turn finish before companion discovery starts.
    QTimer::singleShot(0, this, [this, filePath] {
        if (m_watchedFilePath == filePath && m_document->isOpened()) {
            configureDocsCommentWatcher(QUrl::fromLocalFile(filePath));
        }
    });
}

void Part::unsetFileToWatch()
{
    // A dirty notification may already have armed the delayed reload before
    // the watcher is removed. Never let that stale timer survive a deliberate
    // unwatch (saving, closing, or an explicit documentation render).
    m_dirtyHandler->stop();

    if (m_watchedFilePath.isEmpty()) {
        return;
    }

    m_watcher->removeFile(m_watchedFilePath);

    if (!m_watchedFileSymlinkTarget.isEmpty()) {
        m_watcher->removeFile(m_watchedFileSymlinkTarget);
    }

    m_watchedFilePath.clear();
    m_watchedFileSymlinkTarget.clear();
}

void Part::recordCurrentFileRevision(bool ignoreMatchingDirtySignals)
{
    const QFileInfo fileInfo(localFilePath());
    if (fileInfo.exists()) {
        m_fileLastModified = fileInfo.lastModified();
        m_fileLastSize = fileInfo.size();
        m_ignoreDirtySignalsForCurrentRevision = ignoreMatchingDirtySignals;
    } else {
        m_fileLastModified = QDateTime();
        m_fileLastSize = -1;
        m_ignoreDirtySignalsForCurrentRevision = false;
    }
}

bool Part::currentFileMatchesRecordedRevision() const
{
    const QFileInfo fileInfo(localFilePath());
    return fileInfo.exists() && fileInfo.lastModified() == m_fileLastModified && fileInfo.size() == m_fileLastSize;
}

bool Part::isDocumentArchiveFile(const QMimeType &mime, const QString &fileName)
{
    return mime.inherits(QStringLiteral("application/vnd.kde.okular-archive")) || QFileInfo(fileName).suffix().compare(QLatin1String("okular"), Qt::CaseInsensitive) == 0;
}

Document::OpenResult Part::doOpenFile(const QMimeType &mimeA, const QString &fileNameToOpenA, bool *isCompressedFile)
{
    QMimeDatabase db;
    Document::OpenResult openResult = Document::OpenError;
    bool uncompressOk = true;
    QMimeType mime = mimeA;
    QString fileNameToOpen = fileNameToOpenA;
    KCompressionDevice::CompressionType compressionType = compressionTypeFor(mime.name());
    if (compressionType != KCompressionDevice::None) {
        *isCompressedFile = true;
        uncompressOk = handleCompressed(fileNameToOpen, localFilePath(), compressionType);
        mime = db.mimeTypeForFile(fileNameToOpen);
    } else {
        *isCompressedFile = false;
    }
    const bool documentArchive = isDocumentArchiveFile(mime, fileNameToOpen);

    if (m_swapInsteadOfOpening) {
        m_swapInsteadOfOpening = false;

        if (!uncompressOk) {
            return Document::OpenError;
        }

        if (documentArchive) {
            isDocumentArchive = true;
            if (!m_document->swapBackingFileArchive(fileNameToOpen, url())) {
                return Document::OpenError;
            }
        } else {
            isDocumentArchive = false;
            if (!m_document->swapBackingFile(fileNameToOpen, url())) {
                return Document::OpenError;
            }
        }

        recordCurrentFileRevision();
        return Document::OpenSuccess;
    }

    isDocumentArchive = false;
    if (uncompressOk) {
        if (documentArchive) {
            openResult = m_document->openDocumentArchive(fileNameToOpen, url());
            isDocumentArchive = true;
        } else {
            openResult = m_document->openDocument(fileNameToOpen, url(), mime);
        }
        m_documentOpenWithPassword = false;

        // if the file didn't open correctly it might be encrypted, so ask for a pass
#if HAVE_KWALLET
        QString walletName, walletFolder, walletKey;
        m_document->walletDataForFile(fileNameToOpen, &walletName, &walletFolder, &walletKey);
        bool triedWallet = false;
        KWallet::Wallet *wallet = nullptr;
        bool keep = true;
#endif
        bool firstInput = true;
        while (openResult == Document::OpenNeedsPassword) {
            QString password;

#if HAVE_KWALLET
            // 1.A. try to retrieve the first password from the kde wallet system
            if (!triedWallet && !walletKey.isNull()) {
                const WId parentwid = widget()->effectiveWinId();
                wallet = KWallet::Wallet::openWallet(walletName, parentwid);
                if (wallet) {
                    // use the KPdf folder (and create if missing)
                    if (!wallet->hasFolder(walletFolder)) {
                        wallet->createFolder(walletFolder);
                    }
                    wallet->setFolder(walletFolder);

                    // look for the pass in that folder
                    QString retrievedPass;
                    if (!wallet->readPassword(walletKey, retrievedPass)) {
                        password = retrievedPass;
                    }
                }
                triedWallet = true;
            }
#endif

            // 1.B. if not retrieved, ask the password using the kde password dialog
            if (password.isNull()) {
                QString prompt;
                if (firstInput) {
                    prompt = i18n("Please enter the password to read the document:");
                } else {
                    prompt = i18n("Incorrect password. Try again:");
                }
                firstInput = false;

                // if the user presses cancel, abort opening
#if HAVE_KWALLET
                const KPasswordDialog::KPasswordDialogFlags dlgFlags = wallet ? KPasswordDialog::ShowKeepPassword : KPasswordDialog::KPasswordDialogFlags();
#else
                const KPasswordDialog::KPasswordDialogFlags dlgFlags;
#endif
                KPasswordDialog dlg(widget(), dlgFlags);
                dlg.setWindowTitle(i18n("Document Password"));
                dlg.setPrompt(prompt);
                if (!dlg.exec()) {
                    break;
                }
                password = dlg.password();
#if HAVE_KWALLET
                if (wallet) {
                    keep = dlg.keepPassword();
                }
#endif
            }

            // 2. reopen the document using the password
            if (documentArchive) {
                openResult = m_document->openDocumentArchive(fileNameToOpen, url(), password);
                isDocumentArchive = true;
            } else {
                openResult = m_document->openDocument(fileNameToOpen, url(), mime, password);
            }

            if (openResult == Document::OpenSuccess) {
                m_documentOpenWithPassword = true;

#if HAVE_KWALLET
                // 3. if the password is correct and the user chose to remember it, store it to the wallet
                if (wallet && /*safety check*/ wallet->isOpen() && keep) {
                    wallet->writePassword(walletKey, password);
                }
#endif
            }
        }
    }

    if (openResult == Document::OpenSuccess) {
        recordCurrentFileRevision();
    }
    return openResult;
}

bool Part::openFile()
{
    // Save As swaps in the saved file for the same session; doOpenFile() clears the flag.
    const bool swappingBackingFile = m_swapInsteadOfOpening;
    if (!m_swapInsteadOfOpening) {
        const QWindow *const documentWindow = m_pageView && m_pageView->window() ? m_pageView->window()->windowHandle() : nullptr;
        m_docsDocumentDpi = Okular::Utils::realDpi(documentWindow);
    }
    QList<QMimeType> mimes;
    QString fileNameToOpen = localFilePath();
    QString openedDocsPdfPath;
    const QUrl logicalDocumentUrl = QUrl::fromLocalFile(fileNameToOpen);
    const bool useSidecarReviewAnnotations = shouldSyncDocsComments(logicalDocumentUrl);
    m_document->setUseSidecarReviewAnnotations(useSidecarReviewAnnotations);
    // A PDF outside the Library takes comments too; they are kept on this computer.
    const bool outsidePdf = isOutsidePdf(logicalDocumentUrl);
    m_document->setAnnotationEditingEnabled(useSidecarReviewAnnotations || isWorkspaceThirdPartyPdf(logicalDocumentUrl) || outsidePdf);
    const QString outsideSha256 = outsidePdf ? Thoughts::FeedbackVersion::cachedFileSha256(fileNameToOpen) : QString();
    const QString outsideComments = outsideSha256.isEmpty() ? QString() : Thoughts::OutsidePdfComments::find(fileNameToOpen, outsideSha256);
    // Save As swaps the file under the comments already open: none to restore.
    m_document->setLocalAnnotationsFile(swappingBackingFile ? QString() : outsideComments);
    if (useSidecarReviewAnnotations) {
        openedDocsPdfPath = QFileInfo(fileNameToOpen).canonicalFilePath();
        if (!openedDocsPdfPath.isEmpty()) {
            // Open the resolved immutable generation directly so the identity
            // recorded for comments is exactly the byte stream the generator
            // received, even if current_pdfs advances later.
            fileNameToOpen = openedDocsPdfPath;
        }
    }
    const bool isstdin = url().isLocalFile() && url().fileName() == QLatin1String("-");
    const QFileInfo fileInfo(fileNameToOpen);
    if ((!isstdin) && (!fileInfo.exists())) {
        return false;
    }
    QMimeDatabase db;
    QMimeType pathMime = db.mimeTypeForFile(fileNameToOpen);
    if (!arguments().mimeType().isEmpty()) {
        QMimeType argMime = db.mimeTypeForName(arguments().mimeType());

        // Select the "childmost" mimetype, if none of them
        // inherits the other trust more what pathMime says
        // but still do a second try if that one fails
        if (argMime.inherits(pathMime.name())) {
            mimes << argMime;
        } else if (pathMime.inherits(argMime.name())) {
            mimes << pathMime;
        } else {
            mimes << pathMime << argMime;
        }

        // text is super annoying because it always succeeds when opening so try to make sure
        // that we don't set it as first mime unless we're really sure it is that.
        // If it could be something else based on the content we try that first but only if that content itself
        // is not text or if it's a supported text child like markdown
        if (mimes[0].inherits(QStringLiteral("text/plain"))) {
            const QMimeType contentMime = db.mimeTypeForFile(fileNameToOpen, QMimeDatabase::MatchContent);
            if (!contentMime.inherits(QStringLiteral("text/plain"))) {
                mimes.prepend(contentMime);
            } else if (contentMime.name() != QLatin1String("text/plain")) {
                const QStringList supportedMimes = m_document->supportedMimeTypes();
                if (supportedMimes.contains(contentMime.name())) {
                    mimes.prepend(contentMime);
                }
            }
        }
    } else {
        mimes << pathMime;
    }

    QMimeType mime;
    Document::OpenResult openResult = Document::OpenError;
    bool isCompressedFile = false;
    while (!mimes.isEmpty() && openResult == Document::OpenError) {
        mime = mimes.takeFirst();
        openResult = doOpenFile(mime, fileNameToOpen, &isCompressedFile);
    }

    bool canSearch = m_document->supportsSearching();
    Q_EMIT mimeTypeChanged(mime);

    // update one-time actions
    const bool ok = openResult == Document::OpenSuccess;
    m_outsideCommentsPdfPath = ok && !outsideSha256.isEmpty() ? QFileInfo(fileNameToOpen).absoluteFilePath() : QString();
    m_outsideCommentsSha256 = m_outsideCommentsPdfPath.isEmpty() ? QString() : outsideSha256;
    // Comments found for other bytes at this path, or for these bytes at another path (moved, a new
    // version of the paper): kept for this file from now on.
    if (!m_outsideCommentsPdfPath.isEmpty() && !outsideComments.isEmpty()
        && (outsideComments != Thoughts::OutsidePdfComments::fileFor(outsideSha256)
            || Thoughts::OutsidePdfComments::recordedPath(outsideComments) != QFileInfo(fileNameToOpen).canonicalFilePath())) {
        saveOutsidePdfComments(true);
    } else if (swappingBackingFile && !m_outsideCommentsPdfPath.isEmpty()) {
        saveOutsidePdfComments(true);
    }
    // Annotations embedded in an opened PDF are read-only. After Save As the same
    // annotations are still being edited and are on the undo stack: locking them made
    // undo fail silently (the annotation came back) and freed it twice on close.
    if (ok && !swappingBackingFile) {
        makeExistingAnnotationsReadOnly();
    }
    Q_EMIT enableCloseAction(ok);
    m_find->setEnabled(ok && canSearch);
    m_findNext->setEnabled(ok && canSearch);
    m_findPrev->setEnabled(ok && canSearch);
    Q_EMIT enablePrintAction(ok && m_document->printingSupport() != Okular::Document::NoPrinting);
    m_printPreview->setEnabled(ok && m_document->printingSupport() != Okular::Document::NoPrinting);
    m_showProperties->setEnabled(ok);
    if (m_openContainingFolder) {
        m_openContainingFolder->setEnabled(ok);
    }
    bool hasEmbeddedFiles = ok && m_document->embeddedFiles() && m_document->embeddedFiles()->count() > 0;
    if (m_showEmbeddedFiles) {
        m_showEmbeddedFiles->setEnabled(hasEmbeddedFiles);
    }
    m_topMessage->setVisible(hasEmbeddedFiles && Okular::Settings::showEmbeddedContentMessages());
    m_migrationMessage->setVisible(m_document->isDocdataMigrationNeeded());

    // Warn the user that XFA forms are not supported yet (NOTE: poppler generator only)
    if (ok && Okular::Settings::showEmbeddedContentMessages() && m_document->metaData(QStringLiteral("HasUnsupportedXfaForm")).toBool() == true) {
        m_formsMessage->setText(i18n("This document has XFA forms, which are currently <b>unsupported</b>."));
        m_formsMessage->setIcon(QIcon::fromTheme(QStringLiteral("dialog-warning")));
        m_formsMessage->setMessageType(KMessageWidget::Warning);
        m_formsMessage->setVisible(true);
    }
    // m_pageView->toggleFormsAction() may be null on dummy mode
    else if (ok && Okular::Settings::showEmbeddedContentMessages() && m_pageView->toggleFormsAction() && m_pageView->toggleFormsAction()->isEnabled()) {
        m_formsMessage->setText(i18n("This document has forms. Click on the button to interact with them, or use View -> Show Forms."));
        m_formsMessage->setMessageType(KMessageWidget::Information);
        m_formsMessage->setVisible(true);
    } else {
        m_formsMessage->setVisible(false);
    }
    // The tab's title says which version; this says it is not the Library's PDF.
    if (ok && m_pageView && Thoughts::PdfVersions::isCachedVersion(localFilePath()))
        m_pageView->displayMessage(i18n("An earlier version of this PDF, for reading only: comments and review cards stay with the current version, "
                                        "except the cards retired on this one."));

    if (ok) {
        auto refreshMessage = [this]() {
            KMessageWidget::MessageType messageType;
            QString message;

            std::tie(messageType, message) = SignatureGuiUtils::documentSignatureMessageWidgetText(m_document);

            if (!message.isEmpty()) {
                if (m_embedMode == PrintPreviewMode) {
                    if (Okular::Settings::showEmbeddedContentMessages()) {
                        m_signatureMessage->setText(i18n("All editing and interactive features for this document are disabled. Please save a copy and reopen to edit this document."));
                        m_signatureMessage->setVisible(true);
                    }
                } else {
                    if (Okular::Settings::showEmbeddedContentMessages() || messageType > KMessageWidget::Information) {
                        m_signatureMessage->setMessageType(messageType);
                        m_signatureMessage->setText(message);
                        m_signatureMessage->setVisible(true);
                    }
                }
            }
        };
        refreshMessage();
        for (uint i = 0; i < m_document->pages(); i++) {
            const QList<Okular::FormField *> formFields = m_document->page(i)->formFields();
            for (Okular::FormField *f : formFields) {
                if (f->type() == Okular::FormField::FormSignature) {
                    static_cast<Okular::FormFieldSignature *>(f)->subscribeUpdates(refreshMessage);
                }
            }
        }
    }

    if (m_showPresentation) {
        m_showPresentation->setEnabled(ok);
    }
    if (ok) {
        if (m_exportAs) {
            m_exportFormats = m_document->exportFormats();
            QList<Okular::ExportFormat>::ConstIterator it = m_exportFormats.constBegin();
            QList<Okular::ExportFormat>::ConstIterator itEnd = m_exportFormats.constEnd();
            QMenu *menu = m_exportAs->menu();
            int exportFormatIndex = 0;
            for (; it != itEnd; ++it, ++exportFormatIndex) {
                QAction *formatAction = actionForExportFormat(*it);
                formatAction->setData(exportFormatIndex);
                menu->addAction(formatAction);
            }
        }
#if HAVE_PURPOSE
        if (m_share) {
            m_shareMenu->model()->setInputData(QJsonObject {{QStringLiteral("mimeType"), mime.name()}, {QStringLiteral("urls"), QJsonArray {url().toString()}}});
            m_shareMenu->model()->setPluginType(QStringLiteral("Export"));
            m_shareMenu->reload();
        }
#endif
        if (isCompressedFile) {
            m_realUrl = url();
        }
#ifdef OKULAR_KEEP_FILE_OPEN
        if (keepFileOpen())
            m_keeper->open(fileNameToOpen);
#endif

        // Tries to find the text passed from terminal after the file is open
        if (!m_textToFindOnOpen.isEmpty()) {
            slotShowFindBar();
            m_findBar->startSearch(m_textToFindOnOpen);
            m_textToFindOnOpen = QString();
        }
    }
    if (m_exportAsText) {
        m_exportAsText->setEnabled(ok && m_document->canExportToText());
    }
    if (m_exportAsPdfWithComments) {
        m_exportAsPdfWithComments->setEnabled(ok && mime.inherits(QStringLiteral("application/pdf")));
    }
    if (m_exportAs) {
        m_exportAs->setEnabled(ok);
    }
#if HAVE_PURPOSE
    if (m_share) {
        m_share->setEnabled(ok);
    }
#endif

    // update viewing actions
    updateViewActions();

    m_fileWasRemoved = false;

    if (!ok) {
        // if can't open document, update windows so they display blank contents
        m_pageView->viewport()->update();
        m_thumbnailList->update();
        setUrl(QUrl());
        return false;
    }

    if (const auto reading = Thoughts::ProjectReadingContext::load(fileNameToOpen)) {
        QSettings position;
        const auto key = QStringLiteral("project-reading/") + QString::fromLatin1(QCryptographicHash::hash(reading->reading.id.toUtf8(), QCryptographicHash::Sha256).toHex());
        const int saved = reading->reading.projectPage(position.value(key + QStringLiteral("/document")).toString(), position.value(key + QStringLiteral("/page"), 0).toInt());
        if (saved >= 0) m_document->setViewportPage(saved);
    }
    // set the file to the fileWatcher
    if (url().isLocalFile()) {
        setFileToWatch(localFilePath());
        if (!openedDocsPdfPath.isEmpty()) {
            startDocsPdfIdentityVerification(QUrl::fromLocalFile(localFilePath()), openedDocsPdfPath);
        }
        // Source/comment companions are scheduled by setFileToWatch() once the
        // event loop is running. They must never hold up the first PDF frame.
    }
    // Document feedback once the first frame is up (finding the Library identity may hash the file).
    QTimer::singleShot(0, this, [this] { refreshDocumentFeedback(); });
    // The HTML view follows the opened document (its edition, the remembered PDF | HTML choice).
    // A page's tab shows its page alone, never the edition.
    if (m_htmlReader) {
        m_htmlReader->setDocument(url().isLocalFile() && !m_attachmentTab ? localFilePath() : QString());
    }

    // Contents is available for outlined documents, but Annotations remains
    // the default sidebar page when a document is opened.
    if (m_tocEnabled) {
        m_sidebar->setCurrentItem(m_reviewsWidget);
    }

    // if the 'StartFullScreen' flag is set and we're not in viewer widget mode, or the command line flag was
    // specified, start presentation
    const bool presentationBecauseOfDocumentMetadata = (m_embedMode != ViewerWidgetMode) && m_document->metaData(QStringLiteral("StartFullScreen")).toBool();
    if ((presentationBecauseOfDocumentMetadata || m_cliPresentation) && !m_isReloading) {
        bool goAheadWithPresentationMode = true;
        if (!m_cliPresentation) {
            const QString text = i18n(
                "This document wants to be shown full screen.\n"
                "Leave normal mode and enter presentation mode?");
            const QString caption = i18n("Request to Change Viewing Mode");
            const KGuiItem yesItem = KGuiItem(i18n("Enter Presentation Mode"), QStringLiteral("dialog-ok"));
            const KGuiItem noItem = KGuiItem(i18n("Deny Request"), QStringLiteral("dialog-cancel"));
            const int result = KMessageBox::questionTwoActions(widget(), text, caption, yesItem, noItem);
            if (result == KMessageBox::SecondaryAction) {
                goAheadWithPresentationMode = false;
            }
        }
        m_cliPresentation = false;
        if (goAheadWithPresentationMode) {
            QMetaObject::invokeMethod(this, "slotShowPresentation", Qt::QueuedConnection);
        }
    }
    m_generatorGuiClient = factory() ? m_document->guiClient() : nullptr;
    if (m_generatorGuiClient) {
        factory()->addClient(m_generatorGuiClient);
    }
    if (m_cliPrint) {
        m_cliPrint = false;
        slotPrint();
    } else if (m_cliPrintAndExit) {
        slotPrint();
    }
    return true;
}

bool Part::openUrl(const QUrl &url)
{
    return openUrl(url, false /* swapInsteadOfOpening */);
}

bool Part::openUrl(const QUrl &_url, bool swapInsteadOfOpening)
{
    /* Store swapInsteadOfOpening, so that closeUrl and openFile will be able
     * to read it */
    m_swapInsteadOfOpening = swapInsteadOfOpening;

    // The subsequent call to closeUrl clears the arguments.
    // We want to save them and restore them later.
    const KParts::OpenUrlArguments args = arguments();

    // Close current document if any
    if (!closeUrl()) {
        return false;
    }

    setArguments(args);

    QUrl url(_url);
    if (url.hasFragment()) {
        m_urlWithFragment = _url;
        const QString dest = url.fragment(QUrl::FullyDecoded);
        bool ok = true;
        int page = dest.toInt(&ok);

        if (!ok) {
            const QList<QStringView> parameters = QStringView(dest).split(QLatin1Char('&'));
            for (const QStringView parameter : parameters) {
                if (parameter.startsWith(QLatin1String("page="), Qt::CaseInsensitive)) {
                    page = QStringView(dest).mid(5).toInt(&ok);
                }
            }
        }

        if (ok) {
            Okular::DocumentViewport vp(page - 1);
            vp.rePos.enabled = true;
            vp.rePos.normalizedX = 0;
            vp.rePos.normalizedY = 0;
            vp.rePos.pos = Okular::DocumentViewport::TopLeft;
            m_document->setNextDocumentViewport(vp);
        } else {
            m_document->setNextDocumentDestination(dest);
        }
        url.setFragment(QString());
    } else {
        m_urlWithFragment.clear();
    }

    url = normalizedDocsLibraryUrl(url);

    // this calls in sequence the 'closeUrl' and 'openFile' methods
    bool openOk = KParts::ReadWritePart::openUrl(url);

    if (openOk) {
        m_viewportDirty.pageNumber = -1;

        setWindowTitleFromDocument();
    } else {
        if (m_urlWithFragment.isValid() && m_urlWithFragment.isLocalFile()) {
            openOk = tryOpeningUrlWithFragmentAsName();
        } else {
            resetStartArguments();
            /* TRANSLATORS: Adding the reason (%2) why the opening failed (if any). */
            QString errorMessageString = i18n("Could not open %1. %2", url.toDisplayString(), QStringLiteral("\n%1").arg(m_document->openError()));
            KMessageBox::error(widget(), errorMessageString);
        }
    }

    return openOk;
}

bool Part::tryOpeningUrlWithFragmentAsName()
{
    QUrl url = m_urlWithFragment;
    url.setPath(url.path() + QLatin1Char('#') + url.fragment());
    url.setFragment(QString());
    return openUrl(url);
}

bool Part::queryClose()
{
    saveOutsidePdfComments();
    if (!isModified()) {
        return true;
    }

    // TODO When we get different saving backends we need to query the backend
    // as to if it can save changes even if the open file has been modified,
    // since we only have poppler as saving backend for now we're skipping that check
    if (m_fileLastModified != QFileInfo(localFilePath()).lastModified()) {
        int res;
        if (m_isReloading) {
            res = KMessageBox::warningContinueCancel(widget(),
                                                     xi18nc("@info",
                                                            "The file <filename>%1</filename> has unsaved changes but has been modified by another program. Reloading it "
                                                            "will replace the unsaved changes with the changes made in the other "
                                                            "program.<nl/><nl/>Do you want to continue reloading the file?",
                                                            url().fileName()),
                                                     i18n("File Changed"),
                                                     KGuiItem(i18n("Continue Reloading")), // <- KMessageBox::Continue
                                                     KGuiItem(i18n("Abort Reloading")));
        } else {
            res = KMessageBox::warningContinueCancel(widget(),
                                                     xi18nc("@info",
                                                            "The file <filename>%1</filename> has unsaved changes but has been modified by another program. Closing it "
                                                            "will replace the unsaved changes with the changes made in the other "
                                                            "program.<nl/><nl/>Do you want to continue closing the file?",
                                                            url().fileName()),
                                                     i18n("File Changed"),
                                                     KGuiItem(i18n("Continue Closing")), // <- KMessageBox::Continue
                                                     KGuiItem(i18n("Abort Closing")));
        }
        return res == KMessageBox::Continue;
    }

    const QString operation = m_isReloading ? i18n("reload") : i18n("close");
    const int result = KMessageBox::warningTwoActions(
        widget(),
        i18n("This PDF has unexpected native changes. Thoughts cannot save changes to the open PDF. Export a PDF with Comments first if you need a separate copy, or discard the changes to %1.", operation),
        m_isReloading ? i18n("Reload Document") : i18n("Close Document"),
        KStandardGuiItem::discard(),
        KStandardGuiItem::cancel());
    return result == KMessageBox::PrimaryAction;
}

bool Part::closeUrl(bool promptToSave)
{
    saveOutsidePdfComments();
    if (promptToSave && !queryClose()) {
        return false;
    }
    m_outsideCommentsPdfPath.clear();
    m_outsideCommentsSha256.clear();

    if (m_swapInsteadOfOpening) {
        // If we're swapping the backing file, we don't want to close the
        // current one when openUrl() calls us internally
        return true; // pretend it worked
    }

    m_document->setHistoryClean(true);
    if (m_pageView) m_pageView->documentFeedback()->clear();
    m_reviewUndoIndices.clear();
    m_reviewRedoIndices.clear();
    m_reviewMetadataHistory.clear();
    m_reviewSidecarDeletionHistory.clear();
    m_nextReviewSidecarHistoryToken = -1;

    if (!m_temporaryLocalFile.isNull() && m_temporaryLocalFile != localFilePath()) {
        QFile::remove(m_temporaryLocalFile);
        m_temporaryLocalFile.clear();
    }

    slotHidePresentation();
    Q_EMIT enableCloseAction(false);
    m_find->setEnabled(false);
    m_findNext->setEnabled(false);
    m_findPrev->setEnabled(false);
    m_printPreview->setEnabled(false);
    m_showProperties->setEnabled(false);
    if (m_showEmbeddedFiles) {
        m_showEmbeddedFiles->setEnabled(false);
    }
    if (m_exportAs) {
        m_exportAs->setEnabled(false);
    }
    if (m_exportAsText) {
        m_exportAsText->setEnabled(false);
    }
    if (m_exportAsPdfWithComments) {
        m_exportAsPdfWithComments->setEnabled(false);
    }
    m_exportFormats.clear();
    if (m_exportAs) {
        QMenu *menu = m_exportAs->menu();
        QList<QAction *> acts = menu->actions();
        for (QAction *action : std::as_const(acts)) {
            if (action != m_exportAsPdfWithComments && action != m_exportAsText) {
                menu->removeAction(action);
                delete action;
            }
        }
    }
#if HAVE_PURPOSE
    if (m_share) {
        m_share->setEnabled(false);
        m_shareMenu->clear();
    }
#endif
    if (m_showPresentation) {
        m_showPresentation->setEnabled(false);
    }
    Q_EMIT setWindowCaption(QLatin1String(""));
    Q_EMIT enablePrintAction(false);
    m_realUrl = QUrl();
    if (url().isLocalFile()) {
        unsetFileToWatch();
    }
    clearDocsCommentWatcher();
    m_fileWasRemoved = false;
    if (m_generatorGuiClient) {
        factory()->removeClient(m_generatorGuiClient);
    }
    m_generatorGuiClient = nullptr;
    if (m_htmlReader && !m_isReloading) {
        m_htmlReader->setDocument(QString());
    }
    // An HTML attachment belongs to the PDF that linked it (a reload keeps it).
    if (m_attachmentReader && !m_isReloading) {
        closeHtmlAttachment();
    }
    m_document->closeDocument();
    m_fileLastModified = QDateTime();
    m_fileLastSize = -1;
    m_ignoreDirtySignalsForCurrentRevision = false;
    updateViewActions();
    delete m_tempfile;
    m_tempfile = nullptr;
    if (widget()) {
        if (m_sidebar->isTransientWidgetVisible()) {
            slotHideFindBar();
        } else {
            m_findBar->resetSearch();
        }
        m_migrationMessage->setVisible(false);
        m_topMessage->setVisible(false);
        m_formsMessage->setVisible(false);
        m_signatureMessage->setVisible(false);
    }
#ifdef OKULAR_KEEP_FILE_OPEN
    m_keeper->close();
#endif
    bool r = KParts::ReadWritePart::closeUrl();
    setUrl(QUrl());

    return r;
}

bool Part::closeUrl()
{
    return closeUrl(true);
}

void Part::guiActivateEvent(KParts::GUIActivateEvent *event)
{
    updateViewActions();

    KParts::ReadWritePart::guiActivateEvent(event);

    setWindowTitleFromDocument();

    if (event->activated()) {
        m_pageView->setupActionsPostGUIActivated();
        rebuildBookmarkMenu();
    }
}

void Part::close()
{
    if (m_embedMode == NativeShellMode) {
        closeUrl();
    } else {
        KMessageBox::information(widget(), i18n("This link points to a close document action that does not work when using the embedded viewer."), QString(), QStringLiteral("warnNoCloseIfNotInOkular"));
    }
}

void Part::cannotQuit()
{
    KMessageBox::information(widget(), i18n("This link points to a quit application action that does not work when using the embedded viewer."), QString(), QStringLiteral("warnNoQuitIfNotInOkular"));
}

void Part::slotShowLeftPanel()
{
    bool showLeft = m_showLeftPanel->isChecked();
    Okular::Settings::setShowLeftPanel(showLeft);
    Okular::Settings::self()->save();
    // show/hide left panel
    m_sidebar->setSidebarVisibility(showLeft);
    updateSidebarDisplayActions();
}

void Part::chooseSidebarDisplay(QWidget *item)
{
    if (!m_showLeftPanel || (item && item == m_toc.data() && !m_tocEnabled)) {
        updateSidebarDisplayActions();
        return;
    }
    const bool show = item != nullptr;
    // Choosing a panel during a search closes the search results, like
    // clicking a sidebar tab used to.
    if (m_sidebar->isTransientWidgetVisible()) {
        m_sidebarWasVisibleBeforeSearch = show;
        slotHideFindBar();
    }
    if (item) {
        m_sidebar->setCurrentItem(item);
    }
    if (m_showLeftPanel->isChecked() != show) {
        // trigger(), not setChecked(): the Shell shows or hides its sidebar
        // dock on show_leftpanel's triggered signal, exactly as for F7.
        m_showLeftPanel->trigger();
    } else {
        m_sidebar->setSidebarVisibility(show);
    }
    updateSidebarDisplayActions();
}

void Part::updateSidebarDisplayActions()
{
    if (!m_sidebarDisplay || !m_showLeftPanel) {
        return;
    }
    m_sidebarContentsAction->setEnabled(m_tocEnabled);
    QAction *current = m_sidebarAnnotationsAction;
    if (!m_showLeftPanel->isChecked()) {
        current = m_sidebarHideAction;
    } else if (m_tocEnabled && m_sidebar->currentItem() == m_toc.data()) {
        current = m_sidebarContentsAction;
    } else if (m_historyPanel && m_sidebar->currentItem() == m_historyPanel.data()) {
        current = m_sidebarHistoryAction;
    } else if (m_agentNotesPanel && m_sidebar->currentItem() == m_agentNotesPanel.data()) {
        current = m_sidebarAgentNotesAction;
    }
    if (!current->isChecked()) {
        current->setChecked(true);
    }
}

void Part::slotShowBottomBar()
{
    const bool showBottom = m_showBottomBar->isChecked();
    Okular::Settings::setShowBottomBar(showBottom);
    Okular::Settings::self()->save();
    // show/hide bottom bar
    m_bottomBar->setVisible(showBottom);
}

void Part::slotFileDirty(const QString &path)
{
    const QFileInfo currentFile(localFilePath());
    const bool affectsCurrentFile = path == localFilePath() || currentFile.absolutePath() == path || (currentFile.isSymLink() && currentFile.symLinkTarget() == path);

    // KDirWatch can deliver an event that was queued while Okular atomically
    // replaced its own file. Ignore every such event while the on-disk
    // revision is still exactly the one we recorded after saving. As soon as
    // size or modification time differs, normal external-change handling is
    // restored.
    if (affectsCurrentFile && m_ignoreDirtySignalsForCurrentRevision) {
        if (currentFileMatchesRecordedRevision()) {
            return;
        }
        m_ignoreDirtySignalsForCurrentRevision = false;
    }

    // The beauty of this is that each start cancels the previous one.
    // This means that timeout() is only fired when there have
    // no changes to the file for the last 750 millisecs.
    // This ensures that we don't update on every other byte that gets
    // written to the file.
    if (path == localFilePath()) {
        // Only start watching the file in case if it wasn't removed
        if (QFile::exists(localFilePath())) {
            m_dirtyHandler->start(750);
        } else {
            m_fileWasRemoved = true;
        }
    } else {
        const QFileInfo fi(localFilePath());
        if (fi.absolutePath() == path) {
            // Our parent has been dirtified
            if (!QFile::exists(localFilePath())) {
                m_fileWasRemoved = true;
            } else if (m_fileWasRemoved && QFile::exists(localFilePath())) {
                // we need to watch the new file
                unsetFileToWatch();
                setFileToWatch(localFilePath());
                m_dirtyHandler->start(750);
            }
        } else if (fi.isSymLink() && fi.symLinkTarget() == path) {
            if (QFile::exists(fi.symLinkTarget())) {
                m_dirtyHandler->start(750);
            } else {
                m_fileWasRemoved = true;
            }
        }
    }
}

// Attempt to reload the document, one or more times, optionally from a different URL
bool Part::slotAttemptReload(bool oneShot, const QUrl &newUrl)
{
    if (m_syncUiDeferred) {
        m_syncPdfReloadPending = true;
        if (m_syncPdfReloadSource != url() || !newUrl.isEmpty()) { m_syncPdfReloadTarget = newUrl; }
        m_syncPdfReloadSource = url();
        return true;
    }
    // Skip reload when another reload is already in progress
    if (m_isReloading) {
        return false;
    }
    QScopedValueRollback<bool> rollback(m_isReloading, true);

    bool tocReloadPrepared = false;

    // do the following the first time the file is reloaded
    if (m_viewportDirty.pageNumber == -1) {
        // store the url of the current document
        m_oldUrl = newUrl.isEmpty() ? url() : newUrl;

        // store the current viewport
        m_viewportDirty = m_document->viewport();

        // store the current toolbox pane
        m_dirtyToolboxItem = m_sidebar->currentItem();
        m_wasSidebarVisible = m_sidebar->isSidebarVisible();

        // store if presentation view was open
        m_wasPresentationOpen = (m_presentationWidget != nullptr);

        // preserves the toc state after reload
        m_toc->prepareForReload();
        tocReloadPrepared = true;

        // store the page rotation
        m_dirtyPageRotation = m_document->rotation();

        // inform the user about the operation in progress
        // TODO: Remove this line and integrate reload info in queryClose
        m_pageView->displayMessage(i18n("Reloading the document…"));
    }

    // Closing drops the mask or frame of a card under review with the old pages.
    if (m_pageView) {
        if (const QString cardId = m_pageView->reviewSessionCardId(); !cardId.isEmpty()) m_reloadLostReviewCardId = cardId;
    }

    // close and (try to) reopen the document
    if (!closeUrl()) {
        m_viewportDirty.pageNumber = -1;

        if (tocReloadPrepared) {
            m_toc->rollbackReload();
        }
        return false;
    }

    if (tocReloadPrepared) {
        m_toc->finishReload();
    }

#if HAVE_NEW_SIGNATURE_API
    m_signatureInProgressMessage->setVisible(false);
#endif

    // inform the user about the operation in progress
    m_pageView->displayMessage(i18n("Reloading the document…"));

    bool reloadSucceeded = false;

    if (KParts::ReadWritePart::openUrl(m_oldUrl)) {
        // on successful opening, restore the previous viewport
        if (m_viewportDirty.pageNumber >= (int)m_document->pages()) {
            m_viewportDirty.pageNumber = (int)m_document->pages() - 1;
        }
        m_document->setViewport(m_viewportDirty);
        m_oldUrl = QUrl();
        m_viewportDirty.pageNumber = -1;
        m_document->setRotation(m_dirtyPageRotation);
        if (m_sidebar->currentItem() != m_dirtyToolboxItem) {
            m_sidebar->setCurrentItem(m_dirtyToolboxItem);
        }
        if (m_sidebar->isSidebarVisible() != m_wasSidebarVisible) {
            m_sidebar->setSidebarVisibility(m_wasSidebarVisible);
        }
        if (m_wasPresentationOpen) {
            slotShowPresentation();
        }
        Q_EMIT enablePrintAction(true && m_document->printingSupport() != Okular::Document::NoPrinting);

        reloadSucceeded = true;
        // The review presents its card again on the new pages, where it is now placed.
        if (!m_reloadLostReviewCardId.isEmpty()) Q_EMIT reviewPresentationLost(std::exchange(m_reloadLostReviewCardId, QString()));
    } else if (!oneShot) {
        // start watching the file again (since we dropped it on close)
        setFileToWatch(localFilePath());
        m_dirtyHandler->start(750);
    }

    return reloadSucceeded;
}

void Part::updateViewActions()
{
    bool opened = m_document->pages() > 0;
    if (opened) {
        m_gotoPage->setEnabled(m_document->pages() > 1);

        // Check if you are at the beginning or not
        if (m_document->currentPage() != 0) {
            m_beginningOfDocument->setEnabled(true);
            m_prevPage->setEnabled(true);
        } else {
            if (m_pageView->verticalScrollBar()->value() != 0) {
                // The page isn't at the very beginning
                m_beginningOfDocument->setEnabled(true);
            } else {
                // The page is at the very beginning of the document
                m_beginningOfDocument->setEnabled(false);
            }
            // The document is at the first page, you can go to a page before
            m_prevPage->setEnabled(false);
        }

        if (m_document->pages() == m_document->currentPage() + 1) {
            // If you are at the end, disable go to next page
            m_nextPage->setEnabled(false);
            if (m_pageView->verticalScrollBar()->value() == m_pageView->verticalScrollBar()->maximum()) {
                // If you are the end of the page of the last document, you can't go to the last page
                m_endOfDocument->setEnabled(false);
            } else {
                // Otherwise you can move to the endif
                m_endOfDocument->setEnabled(true);
            }
        } else {
            // If you are not at the end, enable go to next page
            m_nextPage->setEnabled(true);
            m_endOfDocument->setEnabled(true);
        }

        if (m_historyBack) {
            m_historyBack->setEnabled(m_shellNavigationHistory ? m_shellCanGoBack : !m_document->historyAtBegin());
        }
        if (m_historyNext) {
            m_historyNext->setEnabled(m_shellNavigationHistory ? m_shellCanGoForward : !m_document->historyAtEnd());
        }
        m_reload->setEnabled(true);
        if (m_copy) {
            m_copy->setEnabled(true);
        }
        if (m_copyWithoutLineBreaks) {
            m_copyWithoutLineBreaks->setEnabled(true);
        }
        if (m_selectAll) {
            m_selectAll->setEnabled(true);
        }
        if (m_selectCurrentPage) {
            m_selectCurrentPage->setEnabled(true);
        }
    } else {
        m_gotoPage->setEnabled(false);
        m_beginningOfDocument->setEnabled(false);
        m_endOfDocument->setEnabled(false);
        m_prevPage->setEnabled(false);
        m_nextPage->setEnabled(false);
        if (m_historyBack) {
            m_historyBack->setEnabled(false);
        }
        if (m_historyNext) {
            m_historyNext->setEnabled(false);
        }
        m_reload->setEnabled(false);
        if (m_copy) {
            m_copy->setEnabled(false);
        }
        if (m_copyWithoutLineBreaks) {
            m_copyWithoutLineBreaks->setEnabled(false);
        }
        if (m_selectAll) {
            m_selectAll->setEnabled(false);
        }
        if (m_selectCurrentPage) {
            m_selectCurrentPage->setEnabled(false);
        }
    }

    if (factory()) {
        QWidget *menu = factory()->container(QStringLiteral("menu_okular_part_viewer"), this);
        if (menu) {
            menu->setEnabled(opened);
        }

        menu = factory()->container(QStringLiteral("view_orientation"), this);
        if (menu) {
            menu->setEnabled(opened);
        }
    }
    Q_EMIT viewerMenuStateChange(opened);

    updateBookmarksActions();
}

void Part::updateBookmarksActions()
{
    bool opened = m_document->pages() > 0;
    if (opened) {
        m_addBookmark->setEnabled(true);
        if (m_document->bookmarkManager()->isBookmarked(m_document->viewport())) {
            m_addBookmark->setText(i18n("Remove Bookmark"));
            m_addBookmark->setIcon(QIcon::fromTheme(QStringLiteral("bookmark-remove"), QIcon::fromTheme(QStringLiteral("edit-delete-bookmark"))));
            m_renameBookmark->setEnabled(true);
        } else {
            m_addBookmark->setText(m_addBookmarkText);
            m_addBookmark->setIcon(m_addBookmarkIcon);
            m_renameBookmark->setEnabled(false);
        }
    } else {
        m_addBookmark->setEnabled(false);
        m_addBookmark->setText(m_addBookmarkText);
        m_addBookmark->setIcon(m_addBookmarkIcon);
        m_renameBookmark->setEnabled(false);
    }
}

void Part::enableTOC(bool enable)
{
    if (enable == m_tocEnabled) {
        return;
    }

    if (enable) {
        m_sidebar->addItem(m_toc, QIcon::fromTheme(QApplication::isLeftToRight() ? QStringLiteral("format-justify-left") : QStringLiteral("format-justify-right")), i18n("Contents"));
    } else {
        m_sidebar->removeItem(m_toc);
    }

    m_tocEnabled = enable;
    updateSidebarDisplayActions();
}

void Part::slotRebuildBookmarkMenu()
{
    rebuildBookmarkMenu();
}

void Part::enableLayers(bool enable)
{
    Q_UNUSED(enable)
}

void Part::enableSidebarSignaturesItem(bool enable)
{
    Q_UNUSED(enable)
}

void Part::slotShowFindBar()
{
    if (!m_sidebar->isTransientWidgetVisible()) {
        m_sidebarWasVisibleBeforeSearch = m_sidebar->isSidebarVisible();
        m_sidebar->showTransientWidget();
        m_sidebar->setSidebarVisibility(true);
    }
    m_findBar->focusAndSetCursor();
    m_closeFindBar->setEnabled(true);
}

void Part::slotHideFindBar()
{
    if (!m_sidebar->isTransientWidgetVisible()) {
        m_findBar->resetSearch();
        m_closeFindBar->setEnabled(false);
        return;
    }

    m_findBar->maybeHide();
    m_sidebar->hideTransientWidget();
    m_sidebar->setSidebarVisibility(m_sidebarWasVisibleBeforeSearch);
    m_pageView->setFocus();
    m_closeFindBar->setEnabled(false);
}

// BEGIN go to page dialog
class GotoPageDialog : public QDialog
{
    Q_OBJECT

public:
    GotoPageDialog(QWidget *p, int current, int max)
        : QDialog(p)
    {
        setWindowTitle(i18n("Go to Page"));
        buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

        QVBoxLayout *topLayout = new QVBoxLayout(this);
        topLayout->setContentsMargins(6, 6, 6, 6);
        QHBoxLayout *midLayout = new QHBoxLayout();
        spinbox = new QSpinBox(this);
        spinbox->setRange(1, max);
        spinbox->setValue(current);
        spinbox->setFocus();

        slider = new QSlider(Qt::Horizontal, this);
        slider->setRange(1, max);
        slider->setValue(current);
        slider->setSingleStep(1);
        slider->setTickPosition(QSlider::TicksBelow);
        slider->setTickInterval(max / 10);

        connect(slider, &QSlider::valueChanged, spinbox, &QSpinBox::setValue);
        connect(spinbox, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged), slider, &QSlider::setValue);

        QLabel *label = new QLabel(i18n("&Page:"), this);
        label->setBuddy(spinbox);
        topLayout->addWidget(label);
        topLayout->addLayout(midLayout);
        midLayout->addWidget(slider);
        midLayout->addWidget(spinbox);

        // A little bit extra space
        topLayout->addStretch(10);
        topLayout->addWidget(buttonBox);
        spinbox->setFocus();
    }

    int getPage() const
    {
        return spinbox->value();
    }

protected:
    QSpinBox *spinbox;
    QSlider *slider;
    QDialogButtonBox *buttonBox;
};
// END go to page dialog

void Part::slotGoToPage()
{
    GotoPageDialog pageDialog(m_pageView, m_document->currentPage() + 1, m_document->pages());
    if (pageDialog.exec() == QDialog::Accepted) {
        m_document->setViewportPage(pageDialog.getPage() - 1, nullptr, true);
    }
}

void Part::slotPreviousPage()
{
    if (m_document->isOpened() && !(m_document->currentPage() < 1)) {
        m_document->setViewportPage(m_document->currentPage() - 1, nullptr, true);
    }
}

void Part::slotNextPage()
{
    if (m_document->isOpened() && m_document->currentPage() < (m_document->pages() - 1)) {
        m_document->setViewportPage(m_document->currentPage() + 1, nullptr, true);
    }
}

void Part::slotGotoFirst()
{
    if (m_document->isOpened()) {
        m_document->setViewportPage(0, nullptr, true);
        m_beginningOfDocument->setEnabled(false);
    }
}

void Part::slotGotoLast()
{
    if (m_document->isOpened()) {
        DocumentViewport endPage(m_document->pages() - 1);
        endPage.rePos.enabled = true;
        endPage.rePos.normalizedX = 0;
        endPage.rePos.normalizedY = 1;
        endPage.rePos.pos = Okular::DocumentViewport::TopLeft;
        m_document->setViewport(endPage, nullptr, true);
        m_endOfDocument->setEnabled(false);
    }
}

QString Part::navigationViewport() const
{
    return m_document->isOpened() ? m_document->viewport().toString() : QString();
}

bool Part::navigateToDestination(const QString &destination, const QString &viewport, bool recordHistory)
{
    const DocumentViewport target(destination.isEmpty() ? viewport : m_document->metaData(QStringLiteral("NamedViewport"), destination).toString());
    if (!target.isValid() || target.pageNumber >= int(m_document->pages())) {
        return false;
    }
    // History restores must not animate through intermediate pages or create
    // another navigation entry.
    m_document->setViewport(target, nullptr, false, recordHistory);
    return true;
}

std::optional<Part::PdfLinkContext> Part::pdfLinkContext(const Okular::Action *action, int page) const
{
    if (!action || !m_document || !m_document->isOpened()) {
        return std::nullopt;
    }
    const auto pageInfo = [this](int number) { return number >= 0 ? m_document->metaData(QStringLiteral("ThoughtsPageInfo"), number).toMap() : QVariantMap(); };
    // A combined Project PDF knows the document of each page; a single PDF needs a Library identity.
    const bool combined = !pageInfo(0).value(QStringLiteral("document_id")).toString().isEmpty();
    const QString linkPageDocumentId = pageInfo(page).value(QStringLiteral("document_id")).toString();
    PdfLinkContext context;
    if (!combined) {
        if (!url().isLocalFile()) {
            return std::nullopt;
        }
        const QJsonObject identity = Thoughts::DocumentIdentityStore(m_docsRenderRoot).forFile(localFilePath());
        context.sourceDocumentId = identity.value(QStringLiteral("document_id")).toString();
        context.sourcePdfSha256 = identity.value(QStringLiteral("pdf_sha256")).toString();
    }

    if (action->actionType() == Okular::Action::Browse) {
        const QUrl target = static_cast<const Okular::BrowseAction *>(action)->url();
        const QString scheme = target.scheme();
        // Relative and file links open files; thoughts-* and manual-* links are Thoughts' own.
        if (target.isRelative() || scheme == QLatin1String("file") || scheme.startsWith(QLatin1String("thoughts-")) || scheme.startsWith(QLatin1String("manual-"))) {
            return std::nullopt;
        }
        context.link.kind = QStringLiteral("uri");
        context.link.uri = target.toString(QUrl::FullyEncoded);
        if (combined) {
            context.sourceDocumentId = linkPageDocumentId;
        }
    } else if (action->actionType() == Okular::Action::Goto) {
        const auto *go = static_cast<const Okular::GotoAction *>(action);
        // GoToR: links to other files, including \PdfLink, keep their own target.
        if (go->isExternal()) {
            return std::nullopt;
        }
        QString name = go->destinationName();
        Okular::DocumentViewport viewport = go->destViewport();
        if (!name.isEmpty()) {
            const Okular::DocumentViewport named(m_document->metaData(QStringLiteral("NamedViewport"), name).toString());
            if (named.isValid()) {
                viewport = named;
            }
        }
        if (!viewport.isValid() || viewport.pageNumber >= int(m_document->pages())) {
            return std::nullopt;
        }
        const QVariantMap destination = pageInfo(viewport.pageNumber);
        int offset = 0;
        if (combined) {
            // Named destinations carry "<document_id>/"; a link into another document of the
            // Project is a Thoughts link between PDFs, never an override.
            context.sourceDocumentId = destination.value(QStringLiteral("document_id")).toString();
            offset = destination.value(QStringLiteral("page_offset")).toInt();
            const QString prefix = context.sourceDocumentId + QLatin1Char('/');
            if (!name.isEmpty()) {
                if (!name.startsWith(prefix)) {
                    return std::nullopt;
                }
                name.remove(0, prefix.size());
            }
            if (page >= 0 && linkPageDocumentId != context.sourceDocumentId) {
                return std::nullopt;
            }
        }
        context.link.kind = QStringLiteral("goto");
        context.link.destName = name;
        context.link.destPage = viewport.pageNumber - offset;
        // Okular keeps the position normalized from the top left of the page; overrides
        // store PDF points from the bottom left, as in the /XYZ destination.
        const double width = destination.value(QStringLiteral("width")).toDouble();
        const double height = destination.value(QStringLiteral("height")).toDouble();
        if (viewport.rePos.enabled && viewport.rePos.pos == Okular::DocumentViewport::TopLeft && width > 0 && height > 0) {
            const auto points = [](double value) { return std::round(value * 100.0) / 100.0; };
            context.link.destX = points(viewport.rePos.normalizedX * width);
            context.link.destY = points(height * (1.0 - viewport.rePos.normalizedY));
        }
    } else {
        return std::nullopt;
    }
    if (!Thoughts::DocumentIdentityStore::validId(context.sourceDocumentId) || !context.link.isValid()) {
        return std::nullopt;
    }
    if (combined) {
        context.sourcePdfSha256 = Thoughts::DocumentIdentityStore(m_docsRenderRoot).byId(context.sourceDocumentId).value(QStringLiteral("pdf_sha256")).toString();
    }
    return context;
}

QJsonObject Part::pdfLinkOverride(const PdfLinkContext &context) const
{
    return Thoughts::PdfLinkOverrides::findOverride(m_fedoraProjections, context.sourceDocumentId, context.link);
}

bool Part::openPdfLinkOverride(const Okular::Action *action, int page)
{
    // Most clicks: no override anywhere, nothing to describe (that would hash the PDF once).
    if (!Thoughts::PdfLinkOverrides::anyOverrides(m_fedoraProjections)) {
        return false;
    }
    const auto context = pdfLinkContext(action, page);
    if (!context) {
        return false;
    }
    const QJsonObject record = pdfLinkOverride(*context);
    if (record.isEmpty()) {
        return false;
    }
    const QString path = Thoughts::DocumentIdentityStore(m_docsRenderRoot).readablePath(record.value(QStringLiteral("target_document_id")).toString());
    if (path.isEmpty() || !isSignalConnected(QMetaMethod::fromSignal(&Part::requestOpenDocumentLink))) {
        if (m_pageView) {
            m_pageView->displayMessage(i18n("The linked Library document is not on this computer. Opening the original link."), QString(), PageViewMessage::Warning);
        }
        return false;
    }
    // Through the shell, like \PdfLink links, so Back returns to this link.
    Q_EMIT requestOpenDocumentLink(QUrl::fromLocalFile(path), QString(), Okular::DocumentViewport(0).toString());
    return true;
}

void Part::addPdfLinkOverrideActions(QMenu *menu, const Okular::Action *action, int page)
{
    const auto context = pdfLinkContext(action, page);
    if (!menu || !context) {
        return;
    }
    const QJsonObject record = pdfLinkOverride(*context);
    menu->addSeparator();
    if (record.isEmpty()) {
        QAction *link = menu->addAction(QIcon::fromTheme(QStringLiteral("insert-link")), i18n("Link to Library document…"));
        link->setObjectName(QStringLiteral("LinkToLibraryDocumentAction"));
        connect(link, &QAction::triggered, this, [this, context = *context] { choosePdfLinkOverrideTarget(context); });
        return;
    }
    const QJsonObject target = Thoughts::DocumentIdentityStore(m_docsRenderRoot).byId(record.value(QStringLiteral("target_document_id")).toString());
    QString targetName = target.value(QStringLiteral("display_name")).toString().trimmed();
    if (targetName.isEmpty()) {
        targetName = QFileInfo(target.value(QStringLiteral("filename")).toString()).completeBaseName();
    }
    if (QAction *follow = menu->findChild<QAction *>(QStringLiteral("ProcessLinkAction"))) {
        follow->setText(targetName.isEmpty() ? i18n("Open Library Document") : i18n("Open “%1”", targetName));
    }
    QAction *original = menu->addAction(i18n("Open original link"));
    original->setObjectName(QStringLiteral("OpenOriginalLinkAction"));
    connect(original, &QAction::triggered, this, [this, action] { m_document->processAction(action); });
    QAction *change = menu->addAction(QIcon::fromTheme(QStringLiteral("insert-link")), i18n("Change Library document…"));
    change->setObjectName(QStringLiteral("ChangeLibraryDocumentAction"));
    connect(change, &QAction::triggered, this, [this, context = *context] { choosePdfLinkOverrideTarget(context); });
    QAction *remove = menu->addAction(QIcon::fromTheme(QStringLiteral("edit-delete-remove")), i18n("Remove Library link"));
    remove->setObjectName(QStringLiteral("RemoveLibraryLinkAction"));
    connect(remove, &QAction::triggered, this, [this, context = *context] { removePdfLinkOverride(context); });
}

void Part::choosePdfLinkOverrideTarget(const PdfLinkContext &context)
{
    const QJsonObject current = pdfLinkOverride(context);
    const QString currentTarget = current.value(QStringLiteral("target_document_id")).toString();
    Thoughts::PdfLinkOverrideDialog dialog(Thoughts::libraryDocumentChoices(m_docsRenderRoot, context.sourceDocumentId), currentTarget, widget());
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const QString target = dialog.selectedDocumentId();
    if (target.isEmpty() || target == currentTarget) {
        return;
    }
    const QString origin = Thoughts::Platform::desktopOriginId();
    queuePdfLinkOverride(current.isEmpty() ? Thoughts::PdfLinkOverrides::create(context.sourceDocumentId, context.sourcePdfSha256, context.link, target, origin)
                                           : Thoughts::PdfLinkOverrides::retarget(current, target, origin));
}

void Part::removePdfLinkOverride(const PdfLinkContext &context)
{
    // Every override that applies to this link, so that none of them takes over.
    const QString origin = Thoughts::Platform::desktopOriginId();
    for (const QJsonObject &record : Thoughts::PdfLinkOverrides::matchingOverrides(m_fedoraProjections, context.sourceDocumentId, context.link)) {
        if (!queuePdfLinkOverride(Thoughts::PdfLinkOverrides::remove(record, origin))) {
            return;
        }
    }
}

bool Part::queuePdfLinkOverride(const QJsonObject &record)
{
    QString error;
    if (!m_fedoraRepository || !m_fedoraRepository->enqueuePdfLinkOverride(record, &error)) {
        KMessageBox::error(widget(), i18n("The Library link could not be saved: %1", error));
        return false;
    }
    // projectionsChanged has already brought the new override into m_fedoraProjections.
    return true;
}

void Part::setShellNavigationHistory(bool canGoBack, bool canGoForward)
{
    if (m_shellNavigationHistory && m_shellCanGoBack == canGoBack && m_shellCanGoForward == canGoForward) {
        return;
    }
    m_shellNavigationHistory = true;
    m_shellCanGoBack = canGoBack;
    m_shellCanGoForward = canGoForward;
    updateViewActions();
}

void Part::slotHistoryBack()
{
    if (m_shellNavigationHistory) {
        Q_EMIT requestNavigationHistory(false);
    } else {
        m_document->setPrevViewport();
    }
}

void Part::slotHistoryNext()
{
    if (m_shellNavigationHistory) {
        Q_EMIT requestNavigationHistory(true);
    } else {
        m_document->setNextViewport();
    }
}

void Part::slotAddBookmark()
{
    DocumentViewport vp = m_document->viewport();
    if (m_document->bookmarkManager()->isBookmarked(vp)) {
        m_document->bookmarkManager()->removeBookmark(vp);
    } else {
        m_document->bookmarkManager()->addBookmark(vp);
    }
}

void Part::slotRenameBookmark(const DocumentViewport &viewport)
{
    Q_ASSERT(m_document->bookmarkManager()->isBookmarked(viewport));
    if (m_document->bookmarkManager()->isBookmarked(viewport)) {
        KBookmark bookmark = m_document->bookmarkManager()->bookmark(viewport);
        const QString newName = QInputDialog::getText(widget(), i18n("Rename Bookmark"), i18n("Enter the new name of the bookmark:"), QLineEdit::Normal, bookmark.fullText());
        if (!newName.isEmpty()) {
            m_document->bookmarkManager()->renameBookmark(&bookmark, newName);
        }
    }
}

void Part::slotRenameBookmarkFromMenu()
{
    QAction *action = dynamic_cast<QAction *>(sender());
    Q_ASSERT(action);
    if (action) {
        DocumentViewport vp(action->data().toString());
        slotRenameBookmark(vp);
    }
}

void Part::slotRemoveBookmarkFromMenu()
{
    QAction *action = dynamic_cast<QAction *>(sender());
    Q_ASSERT(action);
    if (action) {
        DocumentViewport vp(action->data().toString());
        slotRemoveBookmark(vp);
    }
}

void Part::slotRemoveBookmark(const DocumentViewport &viewport)
{
    Q_ASSERT(m_document->bookmarkManager()->isBookmarked(viewport));
    if (m_document->bookmarkManager()->isBookmarked(viewport)) {
        m_document->bookmarkManager()->removeBookmark(viewport);
    }
}

void Part::slotRenameCurrentViewportBookmark()
{
    slotRenameBookmark(m_document->viewport());
}

bool Part::aboutToShowContextMenu(QMenu * /*menu*/, QAction *action, QMenu *contextMenu)
{
    KBookmarkAction *ba = dynamic_cast<KBookmarkAction *>(action);
    if (ba != nullptr) {
        QAction *separatorAction = contextMenu->addSeparator();
        separatorAction->setObjectName(QStringLiteral("OkularPrivateRenameBookmarkActions"));
        QAction *renameAction = contextMenu->addAction(QIcon::fromTheme(QStringLiteral("edit-rename")), i18n("Rename this Bookmark"), this, &Part::slotRenameBookmarkFromMenu);
        renameAction->setData(ba->property("htmlRef").toString());
        renameAction->setObjectName(QStringLiteral("OkularPrivateRenameBookmarkActions"));
        QAction *deleteAction = contextMenu->addAction(QIcon::fromTheme(QStringLiteral("bookmark-remove"), QIcon::fromTheme(QStringLiteral("edit-delete-bookmark"))), i18n("Remove this Bookmark"), this, &Part::slotRemoveBookmarkFromMenu);
        deleteAction->setData(ba->property("htmlRef").toString());
        deleteAction->setObjectName(QStringLiteral("OkularPrivateRenameBookmarkActions"));
    }
    return ba;
}

void Part::slotPreviousBookmark()
{
    const KBookmark bookmark = m_document->bookmarkManager()->previousBookmark(m_document->viewport());

    if (!bookmark.isNull()) {
        DocumentViewport vp(bookmark.url().fragment(QUrl::FullyDecoded));
        m_document->setViewport(vp, nullptr, true);
    }
}

void Part::slotNextBookmark()
{
    const KBookmark bookmark = m_document->bookmarkManager()->nextBookmark(m_document->viewport());

    if (!bookmark.isNull()) {
        DocumentViewport vp(bookmark.url().fragment(QUrl::FullyDecoded));
        m_document->setViewport(vp, nullptr, true);
    }
}

void Part::slotFind()
{
    // when in presentation mode, there's already a search bar, taking care of
    // the 'find' requests
    if (m_presentationWidget != nullptr) {
        m_presentationWidget->slotFind();
    } else {
        slotShowFindBar();
    }
}

void Part::slotFindNext()
{
    if (!m_sidebar->isTransientWidgetVisible()) {
        slotShowFindBar();
    } else {
        m_findBar->findNext();
    }
}

void Part::slotFindPrev()
{
    if (!m_sidebar->isTransientWidgetVisible()) {
        slotShowFindBar();
    } else {
        m_findBar->findPrev();
    }
}

bool Part::saveFile()
{
    if (m_syncingDocsComments) {
        return true;
    }
    if (!isModified()) {
        return true;
    } else if (!m_outsideCommentsPdfPath.isEmpty()) {
        // A PDF outside the Library is never written: its comments are kept on this computer.
        saveOutsidePdfComments();
        return !isModified();
    } else if (shouldSyncDocsComments(url())) {
        // Library generations are immutable.  Native-only changes such as
        // forms or signatures must be written to a user-selected copy.
        return slotSaveFileAs(false);
    } else {
        return saveAs(url());
    }
}

bool Part::slotSaveFileAs(bool showOkularArchiveAsDefaultFormat)
{
    if (m_embedMode == PrintPreviewMode) {
        return false;
    }

    // Determine the document's mimetype
    QMimeDatabase db;
    QMimeType originalMimeType;
    const QString typeName = m_document->documentInfo().get(DocumentInfo::MimeType);
    if (!typeName.isEmpty()) {
        originalMimeType = db.mimeTypeForName(typeName);
    }

    // What data would we lose if we saved natively?
    bool wontSaveForms, wontSaveAnnotations;
    checkNativeSaveDataLoss(&wontSaveForms, &wontSaveAnnotations);

    const QMimeType okularArchiveMimeType = db.mimeTypeForName(QStringLiteral("application/vnd.kde.okular-archive"));

    // Prepare "Save As" dialog
    const QString originalMimeTypeFilter = i18nc("File type name and pattern", "%1 (%2)", originalMimeType.comment(), originalMimeType.globPatterns().join(QLatin1Char(' ')));
    QStringList archivePatterns = okularArchiveMimeType.globPatterns();
    if (archivePatterns.isEmpty()) {
        archivePatterns << QStringLiteral("*.okular");
    }
    const QString okularArchiveMimeTypeFilter = i18nc("File type name and pattern", "Thoughts document archive (%1)", archivePatterns.join(QLatin1Char(' ')));

    // What format choice should we show as default?
    QString selectedFilter = (isDocumentArchive || showOkularArchiveAsDefaultFormat || wontSaveForms || wontSaveAnnotations) ? okularArchiveMimeTypeFilter : originalMimeTypeFilter;

    QString filter = originalMimeTypeFilter + QStringLiteral(";;") + okularArchiveMimeTypeFilter;

    const QUrl saveUrl = QFileDialog::getSaveFileUrl(widget(), i18n("Save As"), url(), filter, &selectedFilter);

    if (!saveUrl.isValid() || saveUrl.isEmpty()) {
        return false;
    }

    // Has the user chosen to save in .okular archive format?
    const bool saveAsOkularArchive = (selectedFilter == okularArchiveMimeTypeFilter);

    if (saveAsOkularArchive) {
        // Non Plasma file dialogs are terrible and it's very easy to select saving a file as okular archive and call it hello.md
        // and that's bad because it is *not* an .md file so tell the user to fix it
        const QString wantedExtension = QStringLiteral(".okular");
        if (!saveUrl.path().endsWith(wantedExtension, Qt::CaseInsensitive)) {
            const auto button = KMessageBox::questionTwoActions(widget(),
                                                                i18n("You have chosen to save a Thoughts document archive without the file name ending with the '%1' extension. That is not allowed, do you want to choose a new name?", wantedExtension),
                                                                i18n("Unsupported extension"),
                                                                KGuiItem(i18nc("@action:button", "Choose New Name"), QStringLiteral("edit-rename")),
                                                                KStandardGuiItem::cancel());

            return button == KMessageBox::PrimaryAction ? slotSaveFileAs(showOkularArchiveAsDefaultFormat) : false;
        }
    }

    return saveAs(saveUrl, saveAsOkularArchive ? SaveAsOkularArchive : NoSaveAsFlags);
}

bool Part::saveAs(const QUrl &saveUrl)
{
    // Save in the same format (.okular vs native) as the current file
    return saveAs(saveUrl, isDocumentArchive ? SaveAsOkularArchive : NoSaveAsFlags);
}

static QUrl resolveSymlinksIfFileExists(const QUrl &saveUrl)
{
    if (saveUrl.isLocalFile()) {
        const QFileInfo fi(saveUrl.toLocalFile());
        return fi.exists() ? QUrl::fromLocalFile(fi.canonicalFilePath()) : saveUrl;
    } else {
        return saveUrl;
    }
}

bool Part::saveAs(const QUrl &saveUrl, SaveAsFlags flags)
{
    m_lastDocsCommentSyncSucceeded = true;
    m_lastDocsCommentSyncError.clear();

    QElapsedTimer saveTimer;
    saveTimer.start();
    qint64 pdfConversionMs = -1;
    qint64 fileReplacementMs = -1;
    qint64 snapshotMs = -1;
    qint64 backingSwapMs = -1;
    bool savedCurrentPdfInPlace = false;
    bool savedWithNativeChanges = false;
    QString backingMode = QStringLiteral("reload");
    QJsonObject docsCommentSnapshot;
    bool hasDocsCommentSnapshot = false;

    // TODO When we get different saving backends we need to query the backend
    // as to if it can save changes even if the open file has been modified,
    // since we only have poppler as saving backend for now we're skipping that check

    // Don't warn the user about external changes if what actually happened was that
    // the file on disk was deleted for some reason; in this case just go on normally
    // to avoid confusion or data loss.
    // Also don't warn if the file was modified on disk but the user is doing a Save As
    // with a different URL, since the original changed document is safe so there's
    // nothing to warn about.
    {
        const QFileInfo fi(localFilePath());
        if (fi.exists() && m_fileLastModified != fi.lastModified() && saveUrl == realUrl()) {
            const int res = KMessageBox::warningTwoActionsCancel(widget(),
                                                                 xi18nc("@info",
                                                                        "The file <filename>%1</filename> has been modified by another program. If you save now, any "
                                                                        "changes made in the other program will be lost. Are you sure you want to continue?",
                                                                        realUrl().fileName()),
                                                                 i18n("Save - Warning"),
                                                                 KStandardGuiItem::cont(),                // <- KMessageBox::PrimaryAction
                                                                 KGuiItem(i18n("Save a Copy Elsewhere")), // <- KMessageBox::SecondaryAction
                                                                 KStandardGuiItem::cancel());             // <- KMessageBox::Cancel

            if (res == KMessageBox::SecondaryAction) {
                slotSaveFileAs(false);
            }
            if (res != KMessageBox::PrimaryAction) {
                return false;
            }
        }
    }

    bool hasUserAcceptedReload = false;
    if (m_documentOpenWithPassword) {
        const int res = KMessageBox::warningContinueCancel(
            widget(),
            i18n("The current document is protected with a password.<br />In order to save, the file needs to be reloaded. You will be asked for the password again and your undo/redo history will be lost.<br />Do you want to continue?"),
            i18n("Save - Warning"),
            KStandardGuiItem::cont(),
            KStandardGuiItem::cancel());

        switch (res) {
        case KMessageBox::Continue:
            hasUserAcceptedReload = true;
            // do nothing
            break;
        case KMessageBox::Cancel: // User said no to continue, so return true even if save didn't happen otherwise we will get an error
            return true;
        }
    }

    bool setModifiedAfterSave = false;

    QString tmpFileName;
    // don't turn the file_copy that use this to a file_move
    // doesn't work on Windows
    bool deleteTmpFileName = false;
    {
        // Own scope for the QTemporaryFile since we only care about the random name
        // we need to destroy it so the file gets deleted otherwise windows will complain
        // when trying to save over it because the file is still open
        QTemporaryFile tf;
        if (!tf.open()) {
            KMessageBox::information(widget(), i18n("Could not open the temporary file for saving."));
            return false;
        }
        tmpFileName = tf.fileName();
    }

    // Figure out the real save url, for symlinks we don't want to copy over the symlink but over the target file
    const QUrl realSaveUrl = resolveSymlinksIfFileExists(saveUrl);

    // Due to the way we write we can overwrite readonly files so check if it's one and just bail out early
    if (realSaveUrl.isLocalFile()) {
        const QFileInfo fi(realSaveUrl.toLocalFile());
        if (fi.exists() && !fi.isWritable()) {
            KMessageBox::information(widget(), xi18nc("@info", "Could not overwrite <filename>%1</filename> because that file is read-only. Try saving to another location or changing that file's permissions.", realSaveUrl.toLocalFile()));
            return false;
        }
    }

    QScopedPointer<QTemporaryFile> tempFile;
    KIO::Job *copyJob = nullptr; // this will be filled with the job that writes to saveUrl

    // Does the user want a .okular archive?
    if (flags & SaveAsOkularArchive) {
        if (!hasUserAcceptedReload && !m_document->canSwapBackingFile()) {
            const int res = KMessageBox::warningContinueCancel(widget(),
                                                               i18n("After saving, the current document format requires the file to be reloaded. Your undo/redo history will be lost.<br />Do you want to continue?"),
                                                               i18n("Save - Warning"),
                                                               KStandardGuiItem::cont(),
                                                               KStandardGuiItem::cancel());

            switch (res) {
            case KMessageBox::Continue:
                // do nothing
                break;
            case KMessageBox::Cancel: // User said no to continue, so return true even if save didn't happen otherwise we will get an error
                return true;
            }
        }

        if (!m_document->saveDocumentArchive(tmpFileName)) {
            KMessageBox::information(widget(), i18n("File could not be saved in '%1'. Try to save it to another location.", tmpFileName));
            return false;
        }

        copyJob = KIO::file_copy(QUrl::fromLocalFile(tmpFileName), realSaveUrl, -1, KIO::Overwrite);
        deleteTmpFileName = true;
    } else {
        bool wontSaveForms, wontSaveAnnotations;
        checkNativeSaveDataLoss(&wontSaveForms, &wontSaveAnnotations);

        // If something can't be saved in this format, ask for confirmation
        QStringList listOfwontSaves;
        if (wontSaveForms) {
            listOfwontSaves << i18n("Filled form contents");
        }
        if (wontSaveAnnotations) {
            listOfwontSaves << i18n("User annotations");
        }
        if (!listOfwontSaves.isEmpty()) {
            if (saveUrl == url()) {
                // Save
                const QString warningMessageString = i18n("You are about to save changes, but the current file format does not support saving the following elements. Please use the <i>Thoughts document archive</i> format to preserve them.");
                const int result = KMessageBox::warningContinueCancelList(widget(),
                                                                          warningMessageString,
                                                                          listOfwontSaves,
                                                                          i18n("Warning"),
                                                                          KGuiItem(i18n("Save as Thoughts document archive…"), QStringLiteral("document-save-as")), // <- KMessageBox::Continue
                                                                          KStandardGuiItem::cancel());

                switch (result) {
                case KMessageBox::Continue: // -> Save as Okular document archive
                    return slotSaveFileAs(true /* showOkularArchiveAsDefaultFormat */);
                default:
                    return false;
                }
            } else {
                // Save as
                const QString warningMessageString = m_document->canSwapBackingFile() ? i18n(
                                                                                            "You are about to save changes, but the current file format does not support saving the following elements. Please use the <i>Okular document "
                                                                                            "archive</i> format to preserve them. Click <i>Continue</i> to save the document and discard these elements.")
                                                                                      : i18n(
                                                                                            "You are about to save changes, but the current file format does not support saving the following elements. Please use the <i>Okular document "
                                                                                            "archive</i> format to preserve them. Click <i>Continue</i> to save, but you will lose these elements as well as the undo/redo history.");
                const QString continueMessage = m_document->canSwapBackingFile() ? i18n("Continue") : i18n("Continue losing changes");
                const int result = KMessageBox::warningTwoActionsCancelList(widget(),
                                                                            warningMessageString,
                                                                            listOfwontSaves,
                                                                            i18n("Warning"),
                                                                            KGuiItem(i18n("Save as Thoughts document archive…"), QStringLiteral("document-save-as")), // <- KMessageBox::PrimaryAction
                                                                            KGuiItem(continueMessage, QStringLiteral("arrow-right")));                              // <- KMessageBox::SecondaryAction

                switch (result) {
                case KMessageBox::PrimaryAction: // -> Save as Okular document archive
                    return slotSaveFileAs(true /* showOkularArchiveAsDefaultFormat */);
                case KMessageBox::SecondaryAction: // -> Continue
                    setModifiedAfterSave = m_document->canSwapBackingFile();
                    break;
                case KMessageBox::Cancel:
                    return false;
                }
            }
        }

        if (m_document->canSaveChanges()) {
            // If the generator supports saving changes, save them

            QString errorText;
            if (!m_skipDocsCommentSyncForSave && shouldSyncDocsComments(saveUrl)) {
                const int normalizedLegacyComments = normalizeLegacyCrossPageComments();
                if (normalizedLegacyComments > 0) {
                    qInfo() << "Normalized legacy cross-page comment groups:" << normalizedLegacyComments;
                }
            }
            QElapsedTimer conversionTimer;
            conversionTimer.start();
            if (!m_document->saveChanges(tmpFileName, &errorText)) {
                if (errorText.isEmpty()) {
                    KMessageBox::information(widget(), i18n("File could not be saved in '%1'. Try to save it to another location.", tmpFileName));
                } else {
                    KMessageBox::information(widget(), i18n("File could not be saved in '%1'. %2", tmpFileName, errorText));
                }

                return false;
            }
            pdfConversionMs = conversionTimer.elapsed();
            savedWithNativeChanges = true;

            if (!m_skipDocsCommentSyncForSave && shouldSyncDocsComments(saveUrl)) {
                QElapsedTimer snapshotTimer;
                snapshotTimer.start();
                QString snapshotFailureReason;
                if (!writeDocsCommentSnapshot(saveUrl, &docsCommentSnapshot, &snapshotFailureReason)) {
                    qWarning() << "Could not create the native comment snapshot:" << snapshotFailureReason;
                    docsCommentSnapshot = QJsonObject();
                } else {
                    hasDocsCommentSnapshot = true;
                }
                snapshotMs = snapshotTimer.elapsed();
            }

            copyJob = KIO::file_copy(QUrl::fromLocalFile(tmpFileName), realSaveUrl, -1, KIO::Overwrite);
            deleteTmpFileName = true;
        } else {
            // If the generators doesn't support saving changes, we will
            // just copy the original file.

            if (isDocumentArchive) {
                // Special case: if the user is extracting the contents of a
                // .okular archive back to the native format, we can't just copy
                // the open file (which is a .okular). So let's ask to core to
                // extract and give us the real file

                if (!m_document->extractArchivedFile(tmpFileName)) {
                    KMessageBox::information(widget(), i18n("File could not be saved in '%1'. Try to save it to another location.", tmpFileName));
                    return false;
                }

                copyJob = KIO::file_copy(QUrl::fromLocalFile(tmpFileName), realSaveUrl, -1, KIO::Overwrite);
                deleteTmpFileName = true;
            } else {
                // Otherwise just copy the open file.
                // make use of the already downloaded (in case of remote URLs) file,
                // no point in downloading that again
                QUrl srcUrl = QUrl::fromLocalFile(localFilePath());
                // duh, our local file disappeared...
                if (!QFile::exists(localFilePath())) {
                    if (url().isLocalFile()) {
#ifdef OKULAR_KEEP_FILE_OPEN
                        // local file: try to get it back from the open handle on it
                        tempFile.reset(m_keeper->copyToTemporary());
                        if (tempFile)
                            srcUrl = KUrl::fromPath(tempFile->fileName());
#else
                        const QString msg = i18n("Thoughts cannot copy %1 to the specified location.\n\nThe document does not exist anymore.", localFilePath());
                        KMessageBox::error(widget(), msg);
                        return false;
#endif
                    } else {
                        // we still have the original remote URL of the document,
                        // so copy the document from there
                        srcUrl = url();
                    }
                }

                if (srcUrl != saveUrl) {
                    copyJob = KIO::file_copy(srcUrl, realSaveUrl, -1, KIO::Overwrite);
                } else {
                    // Don't do a real copy in this case, just update the timestamps
                    copyJob = KIO::setModificationTime(realSaveUrl, QDateTime::currentDateTime());
                }
            }
        }
    }

    // Stop watching for changes while we write the new file (useful when
    // overwriting)
    if (url().isLocalFile()) {
        unsetFileToWatch();
    }

    const auto deleteTmpFileFunction = [deleteTmpFileName, tmpFileName] {
        Q_ASSERT(deleteTmpFileName == QFile::exists(tmpFileName));
        if (deleteTmpFileName) {
            QFile::remove(tmpFileName);
        }
    };

    KJobWidgets::setWindow(copyJob, widget());
    QElapsedTimer replacementTimer;
    replacementTimer.start();
    if (!copyJob->exec()) {
        KMessageBox::information(widget(), i18n("File could not be saved in '%1'. Error: '%2'. Try to save it to another location.", saveUrl.toDisplayString(), copyJob->errorString()));

        // Restore watcher
        if (url().isLocalFile()) {
            setFileToWatch(localFilePath());
        }

        deleteTmpFileFunction();

        return false;
    }
    fileReplacementMs = replacementTimer.elapsed();
    deleteTmpFileFunction();

    m_document->setHistoryClean(true);

    if (m_document->isDocdataMigrationNeeded()) {
        m_document->docdataMigrationDone();
    }

    bool reloadedCorrectly = true;

    const QFileInfo currentFile(localFilePath());
    const QFileInfo savedFile(saveUrl.toLocalFile());
    const QString currentCanonicalPath = currentFile.canonicalFilePath().isEmpty() ? currentFile.absoluteFilePath() : currentFile.canonicalFilePath();
    const QString savedCanonicalPath = savedFile.canonicalFilePath().isEmpty() ? savedFile.absoluteFilePath() : savedFile.canonicalFilePath();
    const bool savedSameLocalPdf = !isDocumentArchive && savedWithNativeChanges && !m_documentOpenWithPassword && saveUrl.isLocalFile() && url().isLocalFile() && savedFile.suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0 &&
        currentCanonicalPath == savedCanonicalPath;

    // Poppler already contains the successfully saved annotations and forms in
    // memory. For an in-place PDF save, retain that live document and refresh
    // only size-derived metadata instead of closing and reopening the PDF.
    if (savedSameLocalPdf && m_document->refreshBackingFileMetadataAfterSave(saveUrl.toLocalFile())) {
        savedCurrentPdfInPlace = true;
        backingMode = QStringLiteral("retained");
        backingSwapMs = 0;
    } else if (m_document->canSwapBackingFile() && !m_documentOpenWithPassword) {
        QElapsedTimer swapTimer;
        swapTimer.start();
        const bool canUseSilentSwap = !isDocumentArchive && saveUrl.isLocalFile() && url().isLocalFile() && savedFile.suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0 && currentCanonicalPath == savedCanonicalPath;
        savedCurrentPdfInPlace = canUseSilentSwap;
        backingMode = savedSameLocalPdf ? QStringLiteral("swap-fallback") : (canUseSilentSwap ? QStringLiteral("silent-swap") : QStringLiteral("swap"));

        QWidget *currentSidebarItem = m_sidebar->currentItem();
        bool swappedCorrectly = false;
        if (canUseSilentSwap) {
            swappedCorrectly = swapSavedDocumentSilently(saveUrl);
            if (!swappedCorrectly) {
                swappedCorrectly = openUrl(saveUrl, true /* swapInsteadOfOpening */);
            }
        } else {
            // Save As and archive paths keep KParts' ordinary open path.
            swappedCorrectly = openUrl(saveUrl, true /* swapInsteadOfOpening */);
        }

        if (!swappedCorrectly) {
            reloadedCorrectly = false;
        }

        if (m_sidebar->currentItem() != currentSidebarItem) {
            m_sidebar->setCurrentItem(currentSidebarItem);
        }
        backingSwapMs = swapTimer.elapsed();
    } else {
        // If the generator doesn't support swapping file, then just reload
        // the document from the new location
        backingMode = QStringLiteral("reload");
        if (!slotAttemptReload(true, saveUrl)) {
            reloadedCorrectly = false;
        }
    }

    if (reloadedCorrectly && setModifiedAfterSave) {
        m_document->setHistoryClean(false);
    }

    // In case of file swapping errors, close the document to avoid inconsistencies
    if (!reloadedCorrectly) {
        qWarning() << "The document hasn't been reloaded/swapped correctly";
        closeUrl();
    }

    // Set correct permission taking into account the umask value
#ifndef Q_OS_WIN
    const QString saveFilePath = saveUrl.toLocalFile();
    if (QFile::exists(saveFilePath)) {
        const mode_t mask = umask(0);
        umask(mask);
        const mode_t fileMode = 0666 & ~mask;
        chmod(QFile::encodeName(saveFilePath).constData(), fileMode);
    }
#endif

    if (savedCurrentPdfInPlace && reloadedCorrectly) {
        recordCurrentFileRevision(true);
    }

    // Restore watcher
    if (url().isLocalFile()) {
        setFileToWatch(localFilePath());
    }

    if (!m_skipDocsCommentSyncForSave && shouldSyncDocsComments(saveUrl)) {
        QString failureReason;
        const QString saveTiming = QStringLiteral("pdf-convert-ms=%1 file-replace-ms=%2 snapshot-ms=%3 backing-mode=%4 backing-swap-ms=%5").arg(pdfConversionMs).arg(fileReplacementMs).arg(snapshotMs).arg(backingMode).arg(backingSwapMs);
        if (!syncDocsComments(saveUrl, &failureReason, hasDocsCommentSnapshot ? &docsCommentSnapshot : nullptr, saveTiming, saveTimer.elapsed())) {
            m_lastDocsCommentSyncSucceeded = false;
            m_lastDocsCommentSyncError = failureReason;
            errorMessage(i18n("The PDF was saved, but its comments could not be synchronized (%1). "
                              "Log: %2",
                              failureReason,
                              QDir::toNativeSeparators(m_docsCommentSyncLogPath)),
                         15000);
        }
    }

    return true;
}

// If the user wants to save in the original file's format, some features might
// not be available. Find out what cannot be saved in this format
void Part::checkNativeSaveDataLoss(bool *out_wontSaveForms, bool *out_wontSaveAnnotations) const
{
    bool wontSaveForms = false;
    bool wontSaveAnnotations = false;

    if (!m_document->canSaveChanges(Document::SaveFormsCapability)) {
        /* Set wontSaveForms only if there are forms */
        const int pagecount = m_document->pages();

        for (int pageno = 0; pageno < pagecount; ++pageno) {
            const Okular::Page *page = m_document->page(pageno);
            if (!page->formFields().empty()) {
                wontSaveForms = true;
                break;
            }
        }
    }

    if (!m_document->canSaveChanges(Document::SaveAnnotationsCapability)) {
        /* Set wontSaveAnnotations only if there are local annotations */
        const int pagecount = m_document->pages();

        for (int pageno = 0; pageno < pagecount; ++pageno) {
            const QList<Okular::Annotation *> annotations = m_document->page(pageno)->annotations();
            for (const Okular::Annotation *ann : annotations) {
                if (!(ann->flags() & Okular::Annotation::External)) {
                    wontSaveAnnotations = true;
                    break;
                }
            }
            if (wontSaveAnnotations) {
                break;
            }
        }
    }

    *out_wontSaveForms = wontSaveForms;
    *out_wontSaveAnnotations = wontSaveAnnotations;
}

void Part::slotPreferences()
{
    (void)realSlotPreferences();
}

KConfigDialog *Part::realSlotPreferences()
{
    // Create dialog
    PreferencesDialog *dialog = new PreferencesDialog(m_pageView, Okular::Settings::self(), m_embedMode, m_document->editorCommandOverride());
    m_document->fillConfigDialog(dialog);

    dialog->setAttribute(Qt::WA_DeleteOnClose);

    // Show it
    dialog->show();
    return dialog;
}

void Part::slotToggleChangeColors()
{
    slotSetChangeColors(!Okular::SettingsCore::changeColors());
}

void Part::slotSetChangeColors(bool active)
{
    Okular::SettingsCore::setChangeColors(active);
    Okular::Settings::self()->save();
}

void Part::slotAccessibilityPreferences()
{
    // Create dialog
    PreferencesDialog *dialog = new PreferencesDialog(m_pageView, Okular::Settings::self(), m_embedMode, m_document->editorCommandOverride());
    m_document->fillConfigDialog(dialog);

    dialog->setAttribute(Qt::WA_DeleteOnClose);

    // Show it
    dialog->switchToAccessibilityPage();
    dialog->show();
}

void Part::slotAnnotationPreferences()
{
    // Create dialog
    PreferencesDialog *dialog = new PreferencesDialog(m_pageView, Okular::Settings::self(), m_embedMode, m_document->editorCommandOverride());
    m_document->fillConfigDialog(dialog);

    dialog->setAttribute(Qt::WA_DeleteOnClose);

    // Show it
    dialog->switchToAnnotationsPage();
    dialog->show();
}

void Part::slotNewConfig()
{
    // Apply settings here. A good policy is to check whether the setting has
    // changed before applying changes.

    // Watch File
    setWatchFileModeEnabled(Okular::Settings::watchFile());

    // Main View (pageView)
    m_pageView->reparseConfig();

    // update document settings
    m_document->reparseConfig();

    // update TOC settings
    if (m_tocEnabled) {
        m_toc->reparseConfig();
    }

    // update ThumbnailList contents
    if (Okular::Settings::showLeftPanel() && !m_thumbnailList->isHidden()) {
        m_thumbnailList->updateWidgets();
    }

    // update Reviews settings
    m_reviewsWidget->reparseConfig();

    setWindowTitleFromDocument();

    if (m_presentationDrawingActions) {
        m_presentationDrawingActions->reparseConfig();
        if (factory()) {
            factory()->refreshActionProperties();
        }
    }

    // Trigger the shell to sync recent files in File->Open Recent and the RecentItemsModel
    if (m_maxRecentItems != Okular::Settings::maxRecentItems()) {
        m_maxRecentItems = Okular::Settings::maxRecentItems();
        Q_EMIT maxRecentItemsChanged(m_maxRecentItems);
    }
}

void Part::slotPrintPreview()
{
    if (m_document->pages() == 0) {
        return;
    }

    QPrinter printer;
    QString tempFilePattern;

    if (m_document->printingSupport() == Okular::Document::PostscriptPrinting) {
        tempFilePattern = (QDir::tempPath() + QLatin1String("/okular_XXXXXX.ps"));
    } else if (m_document->printingSupport() == Okular::Document::NativePrinting) {
        tempFilePattern = (QDir::tempPath() + QLatin1String("/okular_XXXXXX.pdf"));
    } else {
        return;
    }

    // Generate a temp filename for Print to File, then release the file so generator can write to it
    QTemporaryFile tf(tempFilePattern);
    tf.setAutoRemove(true);
    if (!tf.open()) {
        return;
    }
    printer.setOutputFileName(tf.fileName());
    tf.close();
    setupPrint(printer);
    doPrint(printer);
    if (QFile::exists(printer.outputFileName())) {
        Okular::FilePrinterPreview previewdlg(printer.outputFileName(), widget());
        previewdlg.exec();
    }
}

void Part::slotShowTOCMenu(const Okular::DocumentViewport &vp, const QPoint point, const QString &title)
{
    showMenu(m_document->page(vp.pageNumber), point, title, vp, true);
}

void Part::slotShowMenu(const Okular::Page *page, const QPoint point)
{
    showMenu(page, point);
}

void Part::showTOCInformation(const Okular::DocumentViewport &viewport, const QString &title)
{
    const QUrl pdfUrl = m_document->currentDocument();
    const double x = viewport.rePos.enabled ? std::clamp(viewport.rePos.normalizedX, 0.0, 1.0) : 0.5;
    const double y = viewport.rePos.enabled ? std::clamp(viewport.rePos.normalizedY, 0.0, 1.0) : 0.0;
    const auto source = resolveTexSource(viewport.pageNumber + 1, x, y, title);
    const QString sourcePath = source.success ? source.sourcePath : source.fallbackSourcePath;

    QDialog dialog(widget());
    dialog.setObjectName(QStringLiteral("tocInformationDialog"));
    dialog.setWindowTitle(i18n("Info"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    QStringList details;
    const auto addField = [&](const QString &label, const QString &value, const QString &name) {
        auto *field = new QLineEdit(value, &dialog);
        field->setObjectName(name);
        field->setReadOnly(true);
        field->setCursorPosition(0);
        field->setToolTip(value);
        form->addRow(label, field);
        details.append(label + QLatin1Char(' ') + value);
    };
    addField(i18n("Heading:"), title, QStringLiteral("tocInfoHeading"));
    addField(i18n("Page:"), QString::number(viewport.pageNumber + 1), QStringLiteral("tocInfoPage"));
    addField(i18n("PDF file name:"), pdfUrl.fileName(), QStringLiteral("tocInfoPdfName"));
    addField(i18n("PDF path:"), pdfUrl.isLocalFile() ? QDir::toNativeSeparators(QFileInfo(pdfUrl.toLocalFile()).absoluteFilePath()) : pdfUrl.toDisplayString(), QStringLiteral("tocInfoPdfPath"));
    if (!sourcePath.isEmpty()) {
        addField(i18n("Source file name:"), QFileInfo(sourcePath).fileName(), QStringLiteral("tocInfoSourceName"));
        addField(i18n("Source path:"), QDir::toNativeSeparators(QFileInfo(sourcePath).absoluteFilePath()), QStringLiteral("tocInfoSourcePath"));
        if (source.success && source.line > 0) {
            addField(i18n("Source line:"), QString::number(source.line), QStringLiteral("tocInfoSourceLine"));
        }
    }
    layout->addLayout(form);
    if (!source.success) {
        auto *status = new QLabel(sourcePath.isEmpty() ? i18n("No source file could be identified for this heading.")
                                                     : i18n("This is the containing source file; an exact source line could not be identified."), &dialog);
        status->setWordWrap(true);
        layout->addWidget(status);
        details.append(status->text());
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto *copy = buttons->addButton(i18n("Copy info"), QDialogButtonBox::ActionRole);
    copy->setObjectName(QStringLiteral("tocInfoCopy"));
    connect(copy, &QPushButton::clicked, &dialog, [details] { QApplication::clipboard()->setText(details.join(QLatin1Char('\n'))); });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(720, dialog.sizeHint().height());
    dialog.exec();
}

void Part::showTOCMenu(const Okular::DocumentViewport &viewport, const QPoint &point, const QString &title)
{
    QMenu menu(widget());
    menu.setObjectName(QStringLiteral("tocContextMenu"));
    const bool validPage = viewport.pageNumber >= 0 && m_document->page(viewport.pageNumber);
    auto *info = menu.addAction(QIcon::fromTheme(QStringLiteral("dialog-information")), i18n("Info…"));
    info->setObjectName(QStringLiteral("TOCInformation"));
    info->setEnabled(validPage);
    connect(info, &QAction::triggered, this, [this, viewport, title] { showTOCInformation(viewport, title); });
    if (validPage && m_document->currentDocument().isLocalFile()) {
        auto *source = menu.addAction(QIcon::fromTheme(QStringLiteral("document-edit")), i18n("Open source"));
        source->setObjectName(QStringLiteral("OpenTOCTexSource"));
        connect(source, &QAction::triggered, this, [this, viewport, title] {
            const double x = viewport.rePos.enabled ? std::clamp(viewport.rePos.normalizedX, 0.0, 1.0) : 0.5;
            const double y = viewport.rePos.enabled ? std::clamp(viewport.rePos.normalizedY, 0.0, 1.0) : 0.0;
            const auto source = resolveTexSource(viewport.pageNumber + 1, x, y, title);
            const QFileInfo sourceFile(source.success ? source.sourcePath : source.fallbackSourcePath);
            if (sourceFile.isFile() && sourceFile.isReadable()) {
                // Contents navigation opens the containing file, even when its
                // historical PDF location no longer maps to an exact line.
                Q_EMIT requestOpenCommentSource(sourceFile.canonicalFilePath(), 0);
            } else {
                KMessageBox::error(widget(),
                                   i18n("The source file for this heading could not be opened.\n\n%1", source.failureReason),
                                   i18n("Open source"));
            }
        });
    }
    auto *goTo = menu.addAction(QIcon::fromTheme(QStringLiteral("go-jump")), i18n("Go to page %1", viewport.pageNumber + 1));
    goTo->setObjectName(QStringLiteral("TOCGoToPage"));
    goTo->setEnabled(validPage);
    connect(goTo, &QAction::triggered, this, [this, viewport] { m_document->setViewport(viewport); });

    QMenu documentMenu(i18n("Document"), &menu);
    if (validPage && m_document->currentDocument().isLocalFile()) {
        Q_EMIT requestOutlineDocumentActions(&documentMenu, url().toLocalFile(), viewport.pageNumber + 1, title);
        // Info is always available above, including for ordinary section headings.
        if (auto *duplicateInfo = documentMenu.findChild<QAction *>(QStringLiteral("libraryDocumentInformationAction"))) {
            documentMenu.removeAction(duplicateInfo);
        }
        const auto actions = documentMenu.actions();
        if (std::any_of(actions.cbegin(), actions.cend(), [](QAction *action) { return !action->isSeparator(); })) {
            menu.addSeparator();
            menu.addMenu(&documentMenu);
        }
    }
    menu.addSeparator();
    auto *outline = menu.addMenu(i18n("Outline"));
    outline->setObjectName(QStringLiteral("TOCOutlineMenu"));
    outline->addAction(i18n("Expand section"), m_toc.data(), &TOC::expandRecursively);
    outline->addAction(i18n("Collapse section"), m_toc.data(), &TOC::collapseRecursively);
    outline->addSeparator();
    outline->addAction(i18n("Expand all"), m_toc.data(), &TOC::expandAll);
    outline->addAction(i18n("Collapse all"), m_toc.data(), &TOC::collapseAll);
    menu.exec(point);
}

void Part::showMenu(const Okular::Page *page, const QPoint point, const QString &bookmarkTitle, const Okular::DocumentViewport &vp, bool showTOCActions)
{
    if (m_embedMode == PrintPreviewMode) {
        return;
    }
    if (showTOCActions) {
        showTOCMenu(vp, point, bookmarkTitle);
        return;
    }

    bool reallyShow = false;
    const bool isCurrentPage = page && page->number() == m_document->viewport().pageNumber;

    if (!m_showMenuBarAction) {
        m_showMenuBarAction = findActionInKPartHierarchy<KToggleAction>(KStandardAction::name(KStandardAction::ShowMenubar));
    }
    if (!m_showFullScreenAction) {
        m_showFullScreenAction = findActionInKPartHierarchy<KToggleFullScreenAction>(KStandardAction::name(KStandardAction::FullScreen));
    }

    QMenu popup;

    const QAction *addBookmark = nullptr;
    const QAction *removeBookmark = nullptr;
    const QAction *fitPageWidth = nullptr;
    if (page) {
        popup.addAction(new OKMenuTitle(&popup, i18n("Page %1", page->number() + 1)));
        if (m_thumbnailList->isVisible() && !Okular::Settings::syncThumbnailsViewport()) {
            const QIcon &syncIcon = QIcon::fromTheme(QStringLiteral("emblem-synchronizing"), QIcon::fromTheme(QStringLiteral("view-refresh")));
            popup.addAction(syncIcon, i18n("Sync Thumbnail with Page"), m_thumbnailList.data(), &ThumbnailList::syncThumbnail);
        }
        if ((!isCurrentPage && m_document->bookmarkManager()->isBookmarked(page->number())) || (isCurrentPage && m_document->bookmarkManager()->isBookmarked(m_document->viewport()))) {
            removeBookmark = popup.addAction(QIcon::fromTheme(QStringLiteral("bookmark-remove"), QIcon::fromTheme(QStringLiteral("edit-delete-bookmark"))), i18n("Remove Bookmark"));
        } else {
            addBookmark = popup.addAction(QIcon::fromTheme(QStringLiteral("bookmark-new")), i18n("Add Bookmark"));
        }
        if (m_pageView->canFitPageWidth()) {
            fitPageWidth = popup.addAction(QIcon::fromTheme(QStringLiteral("zoom-fit-best")), i18n("Fit Width"));
        }
        popup.addAction(m_prevBookmark);
        popup.addAction(m_nextBookmark);
        reallyShow = true;
    }

    const int amountOfActions = popup.actions().count();
    if (m_showMenuBarAction && !m_showMenuBarAction->isChecked()) {
        if (m_hamburgerMenuAction) {
            m_hamburgerMenuAction->addToMenu(&popup);
        } else {
            popup.addAction(m_showMenuBarAction);
        }
    }
    if (m_showFullScreenAction && m_showFullScreenAction->isChecked()) {
        popup.addAction(m_showFullScreenAction);
    }
    if (popup.actions().count() > amountOfActions && popup.actions().constLast()->isVisible()) {
        popup.insertAction(popup.actions().at(amountOfActions), new OKMenuTitle(&popup, i18n("Tools")));
        reallyShow = true;
    }

    if (reallyShow) {
        addReadingActions(&popup);
        const QAction *res = popup.exec(point);
        if (res) {
            if (res == addBookmark) {
                if (isCurrentPage && bookmarkTitle.isEmpty()) {
                    m_document->bookmarkManager()->addBookmark(m_document->viewport());
                } else if (!bookmarkTitle.isEmpty()) {
                    m_document->bookmarkManager()->addBookmark(m_document->currentDocument(), vp, bookmarkTitle);
                } else {
                    m_document->bookmarkManager()->addBookmark(page->number());
                }
            } else if (res == removeBookmark) {
                if (isCurrentPage) {
                    m_document->bookmarkManager()->removeBookmark(m_document->viewport());
                } else {
                    m_document->bookmarkManager()->removeBookmark(page->number());
                }
            } else if (res == fitPageWidth) {
                m_pageView->fitPageWidth(page->number());
            }
        }
    }
}

void Part::addReadingActions(QMenu *menu)
{
    menu->addSeparator();
    menu->addAction(m_sidebarAnnotationsAction);
    menu->addAction(m_sidebarContentsAction);
    menu->addAction(m_sidebarHistoryAction);
    menu->addAction(m_sidebarAgentNotesAction);
    menu->addSeparator();
    menu->addAction(actionCollection()->action(QStringLiteral("comments_overview")));
    if (auto *review = findActionInKPartHierarchy<QAction>(QStringLiteral("review_overview"))) {
        menu->addAction(review);
    }
}

template<class Action> Action *Part::findActionInKPartHierarchy(const QString &actionName)
{
    static_assert(std::is_base_of<QAction, Action>::value, "Calling this method to find something other than an Action makes no sense.");
    if (factory()) {
        const QList<KXMLGUIClient *> clients(factory()->clients());
        for (auto client : clients) {
            if (QAction *act = client->actionCollection()->action(actionName)) {
                if (Action *castedAction = qobject_cast<Action *>(act)) {
                    return castedAction;
                }
            }
        }
    }
    return nullptr;
}

KMainWindow *Part::findMainWindow()
{
    auto *potentialMainWindow = parent();
    while (potentialMainWindow) {
        if (auto *mainWindow = qobject_cast<KMainWindow *>(potentialMainWindow)) {
            return mainWindow;
        }
        potentialMainWindow = potentialMainWindow->parent();
    }
    return nullptr;
}

void Part::slotShowProperties()
{
    PropertiesDialog *d = new PropertiesDialog(widget(), m_document);
    connect(d, &QDialog::finished, d, &QObject::deleteLater);
    d->open();
}

void Part::slotShowEmbeddedFiles()
{
    EmbeddedFilesDialog *d = new EmbeddedFilesDialog(widget(), m_document);
    connect(d, &QDialog::finished, d, &QObject::deleteLater);
    d->open();
}

void Part::slotShowPresentation()
{
    if (!m_presentationWidget) {
        m_presentationWidget = new PresentationWidget(widget(), m_document, m_presentationDrawingActions, actionCollection());
    } else {
        m_presentationWidget->activateWindow();
    }
}

void Part::slotHidePresentation()
{
    if (m_presentationWidget) {
        delete m_presentationWidget.data();
    }
}

void Part::slotUpdateHamburgerMenu()
{
    auto ac = actionCollection();

    auto menu = m_hamburgerMenuAction->menu();
    if (!menu) {
        menu = new QMenu(widget());
        m_hamburgerMenuAction->setMenu(menu);
        if (!m_showMenuBarAction) {
            m_showMenuBarAction = findActionInKPartHierarchy<KToggleAction>(KStandardAction::name(KStandardAction::ShowMenubar));
        }
        m_hamburgerMenuAction->setShowMenuBarAction(m_showMenuBarAction);
    } else {
        menu->clear();
    }

    QToolBar *visibleMainToolbar = nullptr;
    if (auto *mainWindow = findMainWindow()) {
        visibleMainToolbar = mainWindow->toolBar();
        if (!visibleMainToolbar->isVisible()) {
            visibleMainToolbar = nullptr;
        }
        const auto toolbars = mainWindow->toolBars();
        for (const auto &toolbar : toolbars) {
            m_hamburgerMenuAction->hideActionsOf(toolbar);
        }

        bool menuAvailable = false; // We already know the menu bar is hidden when this menu is opened.
        // If no menu is available, we want to add actions to the hamburger menu to show them again.
        // The hamburger menu serves as the fallback that is available through the right-click context menu.
        if (visibleMainToolbar && visibleMainToolbar->actions().contains(m_hamburgerMenuAction)) {
            menuAvailable = true;
        }
        if (!menuAvailable) {
            menu->addAction(m_showMenuBarAction);
            if (!visibleMainToolbar) {
                menu->addAction(findActionInKPartHierarchy(QStringLiteral("mainToolBar")));
            }
            menu->addSeparator();
        }
    }

    // When changing actions, keep "Simple by default, powerful when needed" in mind.
    // To retrieve an action, it is fastest to use a direct pointer if available (m_action), otherwise use
    // ac->action(actionName) and if the action isn't in the actionCollection() of this part,
    // use findActionInKPartHierarchy(actionName).
    menu->addAction(findActionInKPartHierarchy(KStandardAction::name(KStandardAction::Open)));
    menu->addAction(findActionInKPartHierarchy(KStandardAction::name(KStandardAction::OpenRecent)));
    menu->addSeparator();
    menu->addAction(ac->action(QStringLiteral("mouse_drag")));
    menu->addAction(m_copy);
    menu->addAction(m_copyWithoutLineBreaks);
    menu->addAction(m_find);
    menu->addAction(m_showLeftPanel);
    menu->addAction(ac->action(KStandardAction::name(KStandardAction::Undo)));
    menu->addAction(ac->action(KStandardAction::name(KStandardAction::Redo)));
    menu->addSeparator();

    menu->addAction(findActionInKPartHierarchy(KStandardAction::name(KStandardAction::Print)));
    menu->addAction(m_printPreview);
    menu->addSeparator();
    menu->addAction(m_showProperties);
    menu->addAction(m_openContainingFolder);
#if HAVE_PURPOSE
    menu->addAction(m_share);
#endif
    menu->addSeparator();

    menu->addAction(ac->action(QStringLiteral("zoom_to")));
    const QMenuBar *menuBar = m_hamburgerMenuAction->menuBar();
    if (menuBar && menuBar->actions().count() < 3) { // 3 to make sure none of the code below can crash.
        menuBar = nullptr;
    }
    auto curatedViewMenu = menu->addMenu(QIcon::fromTheme(QStringLiteral("page-2sides")), menuBar ? menuBar->actions().at(1)->text() : QStringLiteral("View"));
    if (!m_showFullScreenAction) {
        m_showFullScreenAction = findActionInKPartHierarchy<KToggleFullScreenAction>(KStandardAction::name(KStandardAction::FullScreen));
    }
    curatedViewMenu->addAction(m_showFullScreenAction);
    curatedViewMenu->addAction(m_showPresentation);
    curatedViewMenu->addSeparator();
    curatedViewMenu->addAction(findActionInKPartHierarchy(QStringLiteral("view_render_mode")));
    if (auto *viewOrientationMenu = qobject_cast<QMenu *>(factory()->container(QStringLiteral("view_orientation"), this))) {
        curatedViewMenu->addAction(viewOrientationMenu->menuAction());
    }
    curatedViewMenu->addAction(findActionInKPartHierarchy(QStringLiteral("view_trim_mode")));
    m_hamburgerMenuAction->hideActionsOf(curatedViewMenu);

#if HAVE_SPEECH
    auto speakMenu = menu->addMenu(QIcon::fromTheme(QStringLiteral("text-speak")), i18nc("@action:inmenu menu that contains actions to control text to speach", "Speak"));
    speakMenu->addAction(ac->action(QStringLiteral("speak_document")));
    speakMenu->addAction(ac->action(QStringLiteral("speak_from_current_page")));
    speakMenu->addAction(ac->action(QStringLiteral("speak_current_page")));
    speakMenu->addAction(ac->action(QStringLiteral("speak_stop_all")));
    speakMenu->addAction(ac->action(QStringLiteral("speak_pause_resume")));
    m_hamburgerMenuAction->hideActionsOf(speakMenu);
#endif

    // Add the "Settings" menu from the menu bar.
    if (menuBar) {
        menu->addAction(menuBar->actions().at(menuBar->actions().count() - 3));
    }
}

void Part::slotTogglePresentation()
{
    if (m_document->isOpened()) {
        if (!m_presentationWidget) {
            m_presentationWidget = new PresentationWidget(widget(), m_document, m_presentationDrawingActions, actionCollection());
        } else {
            delete m_presentationWidget.data();
        }
    }
}

void Part::reload()
{
    if (m_document->isOpened()) {
        slotReload();
    }
}

void Part::enableStartWithPrint()
{
    m_cliPrint = true;
}

void Part::enableExitAfterPrint()
{
    m_cliPrintAndExit = true;
}

#define kKPlugin QStringLiteral("KPlugin")

void Part::slotAboutBackend()
{
    const KPluginMetaData data = m_document->generatorInfo();
    if (!data.isValid()) {
        return;
    }

    // Here we do a bit of magic because KPluginMetaData doesn't have setters
    // so we get the json info from it, modify it and use that for the KAboutPluginDialog
    // in case the internals of KPluginMetaData change it won't be too bad, at most we're
    // missing the icon or the generator extra description
    QJsonObject rawData = data.rawData();
    const QIcon icon = QIcon::fromTheme(data.iconName());

    // fall back to mime type icon
    if (icon.isNull()) {
        const Okular::DocumentInfo documentInfo = m_document->documentInfo(QSet<DocumentInfo::Key>() << DocumentInfo::MimeType);
        const QString mimeTypeName = documentInfo.get(DocumentInfo::MimeType);
        if (!mimeTypeName.isEmpty()) {
            QMimeDatabase db;
            QMimeType type = db.mimeTypeForName(mimeTypeName);
            if (type.isValid()) {
                QJsonObject kplugin = rawData[kKPlugin].toObject();
                kplugin[QStringLiteral("Icon")] = type.iconName();
                rawData[kKPlugin] = kplugin;
            }
        }
    }

    const QString extraDescription = m_document->metaData(QStringLiteral("GeneratorExtraDescription")).toString();

    if (!extraDescription.isEmpty()) {
        const QString descriptionAndLang = QStringLiteral("Description[%1]").arg(QLocale().name());
        QJsonObject kplugin = rawData[kKPlugin].toObject();
        kplugin[descriptionAndLang] = QStringLiteral("%1\n\n%2").arg(data.description(), extraDescription);
        rawData[kKPlugin] = kplugin;
    }

    KAboutPluginDialog dlg(KPluginMetaData(rawData, data.fileName()), widget());
    dlg.exec();
}

void Part::slotExportAs(QAction *act)
{
    if (!act || !m_document->isOpened()) {
        return;
    }

    QMimeDatabase mimeDatabase;
    if (act == m_exportAsPdfWithComments) {
        const QMimeType pdfMime = mimeDatabase.mimeTypeForName(QStringLiteral("application/pdf"));
        const QString filter = i18nc("File type name and pattern", "%1 (%2)", pdfMime.comment(), QStringLiteral("*.pdf"));
        const QFileInfo sourceInfo(localFilePath());
        const QString baseName = sourceInfo.completeBaseName().isEmpty() ? i18n("document") : sourceInfo.completeBaseName();
        const QString directory = sourceInfo.absoluteDir().exists() ? sourceInfo.absolutePath() : QDir::homePath();
        const QString suggestedPath = QDir(directory).filePath(baseName + QStringLiteral("-with-comments.pdf"));
        const QString fileName = QFileDialog::getSaveFileName(widget(), i18n("Export PDF with Comments"), suggestedPath, filter);
        if (!fileName.isEmpty() && !exportPdfWithComments(QUrl::fromLocalFile(fileName))) {
            KMessageBox::information(widget(), i18n("The PDF with comments could not be exported to '%1'. Try another location.", fileName));
        }
        return;
    }

    if (act == m_exportAsText) {
        const QMimeType textMime = mimeDatabase.mimeTypeForName(QStringLiteral("text/plain"));
        const QString filter = i18nc("File type name and pattern", "%1 (%2)", textMime.comment(), textMime.globPatterns().join(QLatin1Char(' ')));
        const QString fileName = QFileDialog::getSaveFileName(widget(), i18n("Export Plain Text"), QString(), filter);
        if (!fileName.isEmpty() && !m_document->exportToText(fileName)) {
            KMessageBox::information(widget(), i18n("File could not be exported to '%1'. Try another location.", fileName));
        }
        return;
    }

    bool formatIndexOk = false;
    const int formatIndex = act->data().toInt(&formatIndexOk);
    if (!formatIndexOk || formatIndex < 0 || formatIndex >= m_exportFormats.size()) {
        return;
    }
    const QMimeType mimeType = m_exportFormats.at(formatIndex).mimeType();
    const QString filter = i18nc("File type name and pattern", "%1 (%2)", mimeType.comment(), mimeType.globPatterns().join(QLatin1Char(' ')));
    const QString fileName = QFileDialog::getSaveFileName(widget(), i18n("Export Document"), QString(), filter);
    if (!fileName.isEmpty() && !m_document->exportTo(fileName, m_exportFormats.at(formatIndex))) {
        KMessageBox::information(widget(), i18n("File could not be exported to '%1'. Try another location.", fileName));
    }
}

bool Part::exportPdfWithComments(const QUrl &destination)
{
    if (!m_document->isOpened() || !destination.isLocalFile() || destination.toLocalFile().isEmpty()) {
        return false;
    }

    const QFileInfo currentInfo(localFilePath());
    const QFileInfo destinationInfo(destination.toLocalFile());
    const QString currentPath = currentInfo.canonicalFilePath().isEmpty() ? currentInfo.absoluteFilePath() : currentInfo.canonicalFilePath();
    const QString destinationPath = destinationInfo.exists() && !destinationInfo.canonicalFilePath().isEmpty() ? destinationInfo.canonicalFilePath() : destinationInfo.absoluteFilePath();
    if (!currentPath.isEmpty() && currentPath == destinationPath) {
        return false;
    }

    QTemporaryFile temporary(QDir::tempPath() + QStringLiteral("/thoughts-export-XXXXXX.pdf"));
    if (!temporary.open()) {
        return false;
    }
    const QString temporaryPath = temporary.fileName();
    temporary.close();
    if (!temporary.remove()) {
        return false;
    }

    QUrl sourceUrl;
    QString errorText;
    if (m_document->canSaveChanges()) {
        if (!m_document->saveChanges(temporaryPath, &errorText)) {
            QFile::remove(temporaryPath);
            return false;
        }
        sourceUrl = QUrl::fromLocalFile(temporaryPath);
    } else if (QFileInfo::exists(localFilePath())) {
        sourceUrl = QUrl::fromLocalFile(localFilePath());
    } else {
        return false;
    }

    KIO::FileCopyJob *copyJob = KIO::file_copy(sourceUrl, destination, -1, KIO::Overwrite);
    KJobWidgets::setWindow(copyJob, widget());
    const bool exported = copyJob->exec();
    QFile::remove(temporaryPath);
    return exported;
}

void Part::slotReload()
{
    // stop the dirty handler timer, otherwise we may conflict with the
    // auto-refresh system
    m_dirtyHandler->stop();
    slotAttemptReload();
}

static QProcessEnvironment docsToolEnvironment()
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
#ifdef Q_OS_WIN
    // TeX Live and MiKTeX put synctex.exe here, which is on PATH only if
    // their installer was allowed to change it.
    QStringList texEntries;
    const QFileInfoList texLiveYears = QDir(QStringLiteral("C:/texlive")).entryInfoList({QStringLiteral("20*")}, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
    for (const QFileInfo &year : texLiveYears) {
        texEntries.append(QDir(year.absoluteFilePath()).filePath(QStringLiteral("bin/windows")));
    }
    texEntries.append(QDir::home().filePath(QStringLiteral("AppData/Local/Programs/MiKTeX/miktex/bin/x64")));
    texEntries.append(QStringLiteral("C:/Program Files/MiKTeX/miktex/bin/x64"));
    QStringList pathEntries;
    for (const QString &entry : std::as_const(texEntries)) {
        if (QFileInfo(entry).isDir()) {
            pathEntries.append(QDir::toNativeSeparators(entry));
        }
    }
    const QString inheritedWindowsPath = environment.value(QStringLiteral("PATH"));
    if (!inheritedWindowsPath.isEmpty()) {
        pathEntries.append(inheritedWindowsPath);
    }
    environment.insert(QStringLiteral("PATH"), pathEntries.join(QDir::listSeparator()));
#else
    const QStringList requiredPathEntries = {
        QStringLiteral("/Library/TeX/texbin"),
        QStringLiteral("/opt/homebrew/bin"),
        QStringLiteral("/usr/local/bin"),
        QDir::home().filePath(QStringLiteral(".local/bin")),
        QStringLiteral("/Library/Frameworks/Python.framework/Versions/3.13/bin"),
        QStringLiteral("/usr/bin"),
        QStringLiteral("/bin"),
        QStringLiteral("/usr/sbin"),
        QStringLiteral("/sbin"),
    };
    const QString inheritedPath = environment.value(QStringLiteral("PATH"));
    environment.insert(QStringLiteral("PATH"), requiredPathEntries.join(QLatin1Char(':')) + (inheritedPath.isEmpty() ? QString() : QLatin1Char(':') + inheritedPath));
    const QString pythonUserSite = QDir::home().filePath(QStringLiteral("Library/Python/3.13/lib/python/site-packages"));
    const QString inheritedPythonPath = environment.value(QStringLiteral("PYTHONPATH"));
    environment.insert(QStringLiteral("PYTHONPATH"), pythonUserSite + (inheritedPythonPath.isEmpty() ? QString() : QLatin1Char(':') + inheritedPythonPath));
#endif
    return environment;
}

static QByteArray docsCommentRenderFingerprint(const QJsonObject &record)
{
    QJsonObject rendered;
    for (const QString &field : {
             QStringLiteral("id"),
             QStringLiteral("page"),
             QStringLiteral("subtype"),
             QStringLiteral("rect"),
             QStringLiteral("quad_points"),
             QStringLiteral("author"),
             QStringLiteral("modified"),
             QStringLiteral("created"),
             QStringLiteral("contents"),
             QStringLiteral("color"),
             QStringLiteral("flags"),
             QStringLiteral("opacity"),
             QStringLiteral("name"),
             QStringLiteral("fragments"),
             QStringLiteral("created_in_app"),
             QStringLiteral("in_reply_to"),
         }) {
        rendered.insert(field, record.value(field));
    }
    return QJsonDocument(rendered).toJson(QJsonDocument::Compact);
}

static Okular::NormalizedPoint docsNormalizedPoint(double x, double y, double width, double height)
{
    return {
        std::clamp(x / width, 0.0, 1.0),
        std::clamp(1.0 - y / height, 0.0, 1.0),
    };
}

// Source placements belong to an immutable PDF, whereas synchronized annotation
// geometry can still describe an older build. Never apply a point to another PDF.
struct VerifiedCommentPublication {
    QHash<QString, QJsonArray> locations;
    QJsonArray segments;
};

static VerifiedCommentPublication docsVerifiedCommentLocations(const QString &filename, const QString &pdfSha256, const QString &exactArtifactPath = QString())
{
    VerifiedCommentPublication result;
    auto &points = result.locations;
    if (pdfSha256.size() != 64) {
        return result;
    }
    const QDir bundle(QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server/source-bundle")));
    QFile pointerFile(bundle.filePath(QStringLiteral("documents/%1.json").arg(filename)));
    if (exactArtifactPath.isEmpty() && !pointerFile.open(QIODevice::ReadOnly)) return result;
    const QJsonObject pointer = exactArtifactPath.isEmpty() ? QJsonDocument::fromJson(pointerFile.readAll()).object() : QJsonObject();
    const QString digest = exactArtifactPath.isEmpty() ? pointer.value(QStringLiteral("artifact_sha256")).toString() : QFileInfo(exactArtifactPath).fileName();
    if ((exactArtifactPath.isEmpty() && pointer.value(QStringLiteral("schema_version")).toInt() != 1) || !QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(digest).hasMatch()) {
        return result;
    }
    const QString cacheKey = digest + QLatin1Char(':') + pdfSha256;
    static QMutex cacheMutex;
    static QCache<QString, VerifiedCommentPublication> cache(8);
    {
        QMutexLocker locker(&cacheMutex);
        if (const auto *cached = cache.object(cacheKey)) {
            return *cached;
        }
    }
    QFile artifactFile(exactArtifactPath.isEmpty() ? bundle.filePath(QStringLiteral("objects/%1").arg(digest)) : exactArtifactPath);
    if (!artifactFile.open(QIODevice::ReadOnly)) {
        return result;
    }
    const QByteArray contents = artifactFile.readAll();
    const QJsonObject artifact = QJsonDocument::fromJson(contents).object();
    const QJsonObject pdf = artifact.value(QStringLiteral("pdf")).toObject();
    if (QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex() != digest.toLatin1()
        || artifact.value(QStringLiteral("kind")).toString() != QLatin1String("thoughts-document-source-artifact")
        || pdf.value(QStringLiteral("filename")).toString() != filename || pdf.value(QStringLiteral("pdf_sha256")).toString() != pdfSha256
        || artifact.value(QStringLiteral("review_correspondence_schema_version")).toInt() != 2) {
        return result;
    }
    result.segments = pdf.value(QStringLiteral("segments")).toArray();
    const bool compilerModel = artifact.value(QStringLiteral("review_placement_model_version")).toInt() >= 3;
    for (const QJsonValue &value : artifact.value(QStringLiteral("review_correspondences")).toArray()) {
        const QJsonObject correspondence = value.toObject();
        const QString id = correspondence.value(QStringLiteral("item_id")).toString();
        if (correspondence.value(QStringLiteral("kind")).toString() != QLatin1String("comment") || id.isEmpty()) {
            continue;
        }
        const QString state = correspondence.value(QStringLiteral("state")).toString();
        if (compilerModel && !correspondence.value(QStringLiteral("source_targets")).toArray().isEmpty()) {
            points.insert(id, {}); // A known unresolved source target must not fall back to stale PDF geometry.
        }
        if (correspondence.value(QStringLiteral("lifecycle")).toString() != QLatin1String("active")
            || (state != QLatin1String("exact_point") && state != QLatin1String("exact_text"))) {
            continue;
        }
        QJsonArray matching;
        bool valid = true;
        for (const QJsonValue &locationValue : correspondence.value(QStringLiteral("locations")).toArray()) {
            const QJsonObject location = locationValue.toObject();
            if (location.value(QStringLiteral("pdf_filename")).toString() != filename) {
                continue;
            }
            const QJsonObject proof = location.value(QStringLiteral("proof")).toObject();
            const QString kind = location.value(QStringLiteral("placement_kind")).toString();
            bool locationValid = location.value(QStringLiteral("pdf_sha256")).toString() == pdfSha256
                && kind == state && proof.value(QStringLiteral("verified")).toBool()
                && proof.value(QStringLiteral("pdf_sha256")).toString() == pdfSha256
                && location.value(QStringLiteral("page")).toInt() > 0;
            if (compilerModel || proof.value(QStringLiteral("resolver_version")).toInt() >= 3) {
                locationValid = locationValid && Thoughts::validCompilerReviewLocation(location, correspondence.value(QStringLiteral("review_key")).toString(), filename, pdfSha256, pdf.value(QStringLiteral("page_count")).toInt(), correspondence);
            } else if (kind == QLatin1String("exact_point")) {
                locationValid = locationValid && Thoughts::validNormalizedReviewCoordinates(location.value(QStringLiteral("point")).toArray(), 2);
            } else {
                const QJsonArray regions = location.value(QStringLiteral("regions")).toArray();
                locationValid = locationValid && !regions.isEmpty();
                for (const QJsonValue &region : regions) {
                    locationValid = locationValid && Thoughts::validNormalizedReviewCoordinates(region.toArray(), 4);
                }
            }
            valid = valid && locationValid;
            if (locationValid) matching.append(location);
        }
        if (valid && !matching.isEmpty()) {
            points.insert(id, matching);
        }
    }
    {
        QMutexLocker locker(&cacheMutex);
        cache.insert(cacheKey, new VerifiedCommentPublication(result));
    }
    return result;
}

static Okular::Annotation *docsAnnotationFromSidecar(const QJsonObject &record, double pageWidth, double pageHeight)
{
    if (!(pageWidth > 0.0) || !(pageHeight > 0.0)) {
        return nullptr;
    }
    const QJsonArray rect = record.value(QStringLiteral("rect")).toArray();
    if (rect.size() != 4) {
        return nullptr;
    }
    const Okular::NormalizedPoint first = docsNormalizedPoint(rect.at(0).toDouble(), rect.at(1).toDouble(), pageWidth, pageHeight);
    const Okular::NormalizedPoint second = docsNormalizedPoint(rect.at(2).toDouble(), rect.at(3).toDouble(), pageWidth, pageHeight);
    const Okular::NormalizedRect normalizedRect(std::min(first.x, second.x), std::min(first.y, second.y), std::max(first.x, second.x), std::max(first.y, second.y));

    Okular::Annotation *annotation = nullptr;
    const QString subtype = record.value(QStringLiteral("subtype")).toString();
    if (subtype == QLatin1String("Text")) {
        auto *text = new Okular::TextAnnotation();
        text->setTextType(Okular::TextAnnotation::Linked);
        text->setTextIcon(QStringLiteral("Comment"));
        annotation = text;
    } else if (subtype == QLatin1String("Highlight") || subtype == QLatin1String("Underline") || subtype == QLatin1String("Squiggly") || subtype == QLatin1String("StrikeOut")) {
        auto *highlight = new Okular::HighlightAnnotation();
        if (subtype == QLatin1String("Underline")) {
            highlight->setHighlightType(Okular::HighlightAnnotation::Underline);
        } else if (subtype == QLatin1String("Squiggly")) {
            highlight->setHighlightType(Okular::HighlightAnnotation::Squiggly);
        } else if (subtype == QLatin1String("StrikeOut")) {
            highlight->setHighlightType(Okular::HighlightAnnotation::StrikeOut);
        } else {
            highlight->setHighlightType(Okular::HighlightAnnotation::Highlight);
        }
        const QJsonArray points = record.value(QStringLiteral("quad_points")).toArray();
        for (qsizetype offset = 0; offset + 7 < points.size(); offset += 8) {
            Okular::HighlightAnnotation::Quad quad;
            quad.setPoint(docsNormalizedPoint(points.at(offset + 4).toDouble(), points.at(offset + 5).toDouble(), pageWidth, pageHeight), 0);
            quad.setPoint(docsNormalizedPoint(points.at(offset + 6).toDouble(), points.at(offset + 7).toDouble(), pageWidth, pageHeight), 1);
            quad.setPoint(docsNormalizedPoint(points.at(offset + 2).toDouble(), points.at(offset + 3).toDouble(), pageWidth, pageHeight), 2);
            quad.setPoint(docsNormalizedPoint(points.at(offset).toDouble(), points.at(offset + 1).toDouble(), pageWidth, pageHeight), 3);
            highlight->highlightQuads().append(quad);
        }
        if (highlight->highlightQuads().isEmpty()) {
            delete highlight;
            return nullptr;
        }
        annotation = highlight;
    }
    if (!annotation) {
        return nullptr;
    }

    annotation->setUniqueName(record.value(QStringLiteral("id")).toString());
    annotation->setCreatorApplication(record.value(QStringLiteral("created_in_app")).toString());
    annotation->setBoundingRectangle(normalizedRect);
    annotation->setAuthor(record.value(QStringLiteral("author")).toString());
    annotation->setContents(record.value(QStringLiteral("contents")).toString());
    const QDateTime modified = Okular::DocsCommentSynchronizer::parseCommentDate(record.value(QStringLiteral("modified")));
    const QDateTime created = Okular::DocsCommentSynchronizer::parseCommentDate(record.value(QStringLiteral("created")));
    const QDateTime now = QDateTime::currentDateTime();
    if (modified.isValid() && modified <= now) {
        annotation->setModificationDate(modified);
    }
    if (created.isValid() && created <= now && (!modified.isValid() || created <= modified)) {
        annotation->setCreationDate(created);
    }
    const QJsonArray color = record.value(QStringLiteral("color")).toArray();
    if (color.size() >= 3) {
        const auto channel = [&color](int index) {
            const double value = color.at(index).toDouble();
            return std::clamp(value > 1.0 ? value / 255.0 : value, 0.0, 1.0);
        };
        annotation->style().setColor(QColor::fromRgbF(channel(0), channel(1), channel(2)));
    }
    if (record.contains(QStringLiteral("opacity"))) {
        annotation->style().setOpacity(std::clamp(record.value(QStringLiteral("opacity")).toDouble(1.0), 0.0, 1.0));
    }
    if (record.contains(QStringLiteral("flags"))) {
        const int pdfFlags = record.value(QStringLiteral("flags")).toInt();
        int flags = 0;
        if (!(pdfFlags & 4)) {
            flags |= Okular::Annotation::DenyPrint;
        }
        if (pdfFlags & 2) {
            flags |= Okular::Annotation::Hidden;
        }
        if (pdfFlags & 8) {
            flags |= Okular::Annotation::FixedSize;
        }
        if (pdfFlags & 16) {
            flags |= Okular::Annotation::FixedRotation;
        }
        if (pdfFlags & 64) {
            flags |= Okular::Annotation::DenyWrite;
        }
        if (pdfFlags & 128) {
            flags |= Okular::Annotation::DenyDelete;
        }
        if (pdfFlags & 256) {
            flags |= Okular::Annotation::ToggleHidingOnMouse;
        }
        annotation->setFlags(flags & ~(Okular::Annotation::DenyWrite | Okular::Annotation::DenyDelete));
    }
    return annotation;
}

static QJsonArray docsPhysicalCommentsFromSidecar(const QJsonObject &record)
{
    const QString logicalId = record.value(QStringLiteral("id")).toString();
    const QJsonArray fragments = record.value(QStringLiteral("fragments")).toArray();
    if (logicalId.isEmpty() || fragments.isEmpty()) {
        return {record};
    }

    QJsonArray physical;
    for (qsizetype index = 0; index < fragments.size(); ++index) {
        if (!fragments.at(index).isObject()) {
            continue;
        }
        QJsonObject fragment = record;
        fragment.remove(QStringLiteral("fragments"));
        const QJsonObject geometry = fragments.at(index).toObject();
        for (auto it = geometry.begin(); it != geometry.end(); ++it) {
            fragment.insert(it.key(), it.value());
        }
        fragment.insert(QStringLiteral("id"), index == 0 ? logicalId : Thoughts::makeCommentFragmentId(logicalId, static_cast<int>(index)));
        if (index > 0) {
            fragment.insert(QStringLiteral("contents"), QString());
        }
        physical.append(fragment);
    }
    return physical.isEmpty() ? QJsonArray {record} : physical;
}

static QString docsReplyId(QString id)
{
    if (id.startsWith(QLatin1String("id:"))) {
        id.remove(0, 3);
    }
    const std::optional<Thoughts::CommentFragmentId> fragment = Thoughts::parseCommentFragmentId(id);
    return fragment ? fragment->logicalId : id;
}

static QDateTime docsReplyOrderDate(const QJsonObject &record)
{
    for (const QString &field : {QStringLiteral("created"), QStringLiteral("modified")}) {
        const QDateTime date = Okular::DocsCommentSynchronizer::parseCommentDate(record.value(field));
        if (date.isValid()) {
            return date;
        }
    }
    return {};
}

static void connectDocsReplyThreads(const QHash<QString, QJsonObject> &desired, const QHash<QString, QPair<int, Okular::Annotation *>> &existing)
{
    QHash<QString, QJsonObject> recordsById;
    QHash<QString, Okular::Annotation *> annotationsById;
    for (auto it = desired.cbegin(); it != desired.cend(); ++it) {
        const QString id = docsReplyId(it.key());
        const auto annotation = existing.constFind(id);
        if (id.isEmpty() || annotation == existing.cend() || !annotation->second) {
            continue;
        }
        recordsById.insert(id, it.value());
        annotationsById.insert(id, annotation->second);
        annotation->second->setPreviousAnnotation(nullptr);
    }

    QHash<QString, QString> rootById;
    for (auto it = recordsById.cbegin(); it != recordsById.cend(); ++it) {
        const QString id = it.key();
        const QString canonicalRoot = it->value(QStringLiteral("thread_root_id")).toString();
        if (!canonicalRoot.isEmpty() && recordsById.contains(canonicalRoot)) {
            rootById.insert(id, canonicalRoot);
            continue;
        }
        QString currentId = id;
        QString rootId = id;
        QSet<QString> visited;
        bool validThread = true;
        while (validThread) {
            if (visited.contains(currentId)) {
                validThread = false;
                break;
            }
            visited.insert(currentId);
            const auto current = recordsById.constFind(currentId);
            if (current == recordsById.cend()) {
                validThread = false;
                break;
            }
            const QString parentId = docsReplyId(current->value(QStringLiteral("in_reply_to")).toString());
            if (parentId.isEmpty()) {
                rootId = currentId;
                break;
            }
            if (!recordsById.contains(parentId)) {
                validThread = false;
                break;
            }
            currentId = parentId;
        }
        rootById.insert(id, validThread ? rootId : id);
    }

    QHash<QString, QStringList> repliesByRoot;
    for (auto it = rootById.cbegin(); it != rootById.cend(); ++it) {
        if (it.key() != it.value()) {
            repliesByRoot[it.value()].append(it.key());
        }
    }
    for (auto replies = repliesByRoot.begin(); replies != repliesByRoot.end(); ++replies) {
        std::sort(replies->begin(), replies->end(), [&recordsById](const QString &left, const QString &right) {
            const QDateTime leftDate = docsReplyOrderDate(recordsById.value(left));
            const QDateTime rightDate = docsReplyOrderDate(recordsById.value(right));
            if (leftDate != rightDate) {
                if (!leftDate.isValid()) {
                    return false;
                }
                if (!rightDate.isValid()) {
                    return true;
                }
                return leftDate < rightDate;
            }
            return left < right;
        });
        Okular::Annotation *previous = annotationsById.value(replies.key());
        if (!previous) {
            continue;
        }
        for (const QString &replyId : std::as_const(*replies)) {
            Okular::Annotation *reply = annotationsById.value(replyId);
            if (!reply) {
                continue;
            }
            reply->setPreviousAnnotation(previous);
            reply->setFlags(reply->flags() | Okular::Annotation::Hidden);
            previous = reply;
        }
    }
}

void Part::clearDocsCommentWatcher()
{
    if (m_docsCommentRefreshTimer) {
        m_docsCommentRefreshTimer->stop();
    }
    m_docsCompanionActive = false;
    if (m_docsCommentMutationCaptureTimer) {
        m_docsCommentMutationCaptureTimer->stop();
    }
    if (m_docsCommentMutationFlushTimer) {
        m_docsCommentMutationFlushTimer->stop();
    }
    m_pendingReviewAnnotationId.clear();
    if (!m_applyingCommentStorage) {
        m_commentService.reset(); m_legacyCommentStates.clear();
        m_commentUndoTokens.clear(); m_commentRedoTokens.clear(); m_commentHistoryDocumentIndex.clear();
    }
    delete m_unplacedCommentPanel.data();
    if (m_reviewsWidget) m_reviewsWidget->setUnplacedComments({});
    if (m_pageView) m_pageView->setCommentSourceConnectors({});
    m_docsCommentPdfPath.clear();
    m_docsCommentFingerprint.clear();
    m_docsSourceReconciledCommentFingerprint.clear();
    m_docsAppliedCommentFingerprints.clear();
    m_docsCommentRecords.clear();
    m_docsProjectedAnnotations.clear();
    m_pendingDocsCommentMutationIds.clear();
    m_docsNewCommentIds.clear();
    m_transientEmptyDocsCommentIds.clear();
    m_pendingDocsCommentPreHistoryIndex = -1;
    m_pendingDocsCommentWasClean = false;
    m_docsOpenedPdfPath.clear();
    m_docsOpenedPdfSha256.clear();
    m_docsOpenedPdfIdentityVerified = false;
    m_docsPreparedCommentLoad = {};
    m_docsCommentRefreshQueued = false;
    clearDocsSourceWatcher();
    if (m_reviewsWidget) {
        m_reviewsWidget->setDocsSourceLinkStatus(Reviews::DocsSourceLinkState::NoDocument);
    }
}

void Part::configureDocsCommentWatcher(const QUrl &documentUrl)
{
    if (!shouldSyncDocsComments(documentUrl)) {
        clearDocsCommentWatcher();
        if (m_reviewsWidget && documentUrl.isValid()) {
            m_reviewsWidget->setDocsSourceLinkStatus(Reviews::DocsSourceLinkState::NotLinked);
        }
        return;
    }
    const QString pdfPath = QFileInfo(documentUrl.toLocalFile()).absoluteFilePath();
    const bool changedDocument = m_docsCommentPdfPath != pdfPath;
    const bool sourceNeedsConfiguration = m_docsSourcePdfPath != pdfPath;
    if (changedDocument) {
        clearDocsCommentWatcher();
        m_docsCommentPdfPath = pdfPath;
        startDocsPdfIdentityVerification(documentUrl);
    } else if (!m_docsOpenedPdfIdentityVerified && !m_docsPdfIdentityWatcher->isRunning()) {
        startDocsPdfIdentityVerification(documentUrl);
    }

    m_docsCompanionActive = true;
    followDocsChanges();
    scheduleDocsCommentRefresh();
    if (changedDocument || sourceNeedsConfiguration) {
        configureDocsSourceWatcher(documentUrl, true);
    }
}

void Part::scheduleDocsCommentRefresh()
{
    if (m_docsCommentRefreshTimer && !m_docsCommentPdfPath.isEmpty()) {
        m_docsCommentRefreshQueued = true;
        m_docsCommentRefreshTimer->start();
    }
}

void Part::startDocsCommentLoad()
{
    if (m_syncUiDeferred) { m_docsCommentRefreshQueued = true; return; }
    if (m_docsCommentPdfPath.isEmpty() || !m_document->isOpened()) {
        m_docsCommentRefreshQueued = false;
        return;
    }
    if (m_applyingDocsSidecar || m_syncingDocsComments || isModified()) {
        m_docsCommentRefreshQueued = true;
        return;
    }
    if (Thoughts::ProjectReadingContext::load(m_docsCommentPdfPath)) {
        m_docsUseFedoraForNextCommentRefresh = m_hasFedoraProjectionSnapshot;
        refreshDocsCommentsFromSidecar();
        return;
    }
    const QString pendingStem = QFileInfo(m_docsCommentPdfPath).completeBaseName();
    if ((m_docsCommentMutationStore && m_docsCommentMutationStore->hasPending(m_docsRenderRoot, pendingStem)) || m_docsCommentMutationFlushWatcher->isRunning()) {
        m_docsCommentRefreshQueued = true;
        return;
    }
    const QString pdfPath = QFileInfo(m_docsCommentPdfPath).absoluteFilePath();
    if (QFileInfo(m_document->currentDocument().toLocalFile()).absoluteFilePath() != pdfPath) {
        m_docsCommentRefreshQueued = false;
        return;
    }
    if (m_hasFedoraProjectionSnapshot) {
        m_docsUseFedoraForNextCommentRefresh = true;
        refreshDocsCommentsFromSidecar();
        return;
    }
    if (m_docsCommentLoadWatcher->isRunning()) {
        m_docsCommentRefreshQueued = true;
        return;
    }

    m_docsCommentRefreshQueued = false;
    const QString docsRoot = m_docsRenderRoot;
    const QString pdfStem = QFileInfo(pdfPath).completeBaseName();
    QPromise<QJsonObject> promise;
    const QFuture<QJsonObject> future = promise.future();
    promise.start();
    m_docsCommentLoadWatcher->setFuture(future);
    QThreadPool::globalInstance()->start([promise = std::move(promise), docsRoot, pdfPath, pdfStem]() mutable {
        QString error;
        QByteArray fingerprint;
        QSet<QString> suppressedIds;
        const QList<QJsonObject> visible = DocsCommentSynchronizer::visibleSidecarComments(docsRoot, pdfStem, &fingerprint, &error, &suppressedIds);
        QJsonArray encodedVisible;
        for (const QJsonObject &record : visible) {
            encodedVisible.append(record);
        }
        QStringList orderedSuppressedIds(suppressedIds.begin(), suppressedIds.end());
        std::sort(orderedSuppressedIds.begin(), orderedSuppressedIds.end());
        QJsonArray encodedSuppressedIds;
        for (const QString &id : std::as_const(orderedSuppressedIds)) {
            encodedSuppressedIds.append(id);
        }
        const QString sidecarPath = QDir(docsRoot).filePath(QStringLiteral("comments/%1_comments.jsonl").arg(pdfStem));
        promise.addResult(QJsonObject {
            {QStringLiteral("pdf_path"), pdfPath},
            {QStringLiteral("visible"), encodedVisible},
            {QStringLiteral("suppressed_ids"), encodedSuppressedIds},
            {QStringLiteral("fingerprint"), QString::fromLatin1(fingerprint.toHex())},
            {QStringLiteral("sidecar_content_hash"), QString::fromLatin1(docsSourceFingerprint(sidecarPath).toHex())},
            {QStringLiteral("error"), error},
        });
        promise.finish();
    });
}

void Part::finishDocsCommentLoad()
{
    if (m_syncUiDeferred) { m_docsCommentRefreshQueued = true; return; }
    const bool rerunRequested = m_docsCommentRefreshQueued;
    const QJsonObject prepared = m_docsCommentLoadWatcher->result();
    const QString preparedPath = QFileInfo(prepared.value(QStringLiteral("pdf_path")).toString()).absoluteFilePath();
    const bool sameDocument = m_document->isOpened() && QFileInfo(m_document->currentDocument().toLocalFile()).absoluteFilePath() == preparedPath && QFileInfo(m_docsCommentPdfPath).absoluteFilePath() == preparedPath;
    if (sameDocument) {
        const QString sidecarPath = QDir(m_docsRenderRoot).filePath(QStringLiteral("comments/%1_comments.jsonl").arg(QFileInfo(preparedPath).completeBaseName()));
        const QString currentHash = QString::fromLatin1(docsSourceFingerprint(sidecarPath).toHex());
        if (currentHash == prepared.value(QStringLiteral("sidecar_content_hash")).toString()) {
            m_docsPreparedCommentLoad = prepared;
            refreshDocsCommentsFromSidecar();
        } else {
            m_docsPreparedCommentLoad = {};
            scheduleDocsCommentRefresh();
        }
    }
    if (sameDocument && rerunRequested) {
        scheduleDocsCommentRefresh();
    }
}

void Part::refreshDocsCommentsFromSidecar()
{
    if (m_syncUiDeferred) { m_docsCommentRefreshQueued = true; return; }
    QElapsedTimer projectionTimer;
    projectionTimer.start();
    if (m_docsCommentPdfPath.isEmpty() || !m_document->isOpened()) {
        m_docsCommentRefreshQueued = false;
        return;
    }
    if (m_applyingDocsSidecar || m_syncingDocsComments || isModified()) {
        m_docsCommentRefreshQueued = true;
        return;
    }
    const QFileInfo pdfInfo(m_docsCommentPdfPath);
    if (QFileInfo(m_document->currentDocument().toLocalFile()).absoluteFilePath() != pdfInfo.absoluteFilePath()) {
        m_docsCommentRefreshQueued = false;
        return;
    }

    QString error;
    QByteArray fingerprint;
    QSet<QString> suppressedIds;
    QList<QJsonObject> visible;
    const QString pdfStem = pdfInfo.completeBaseName();
    const auto reading = Thoughts::ProjectReadingContext::load(pdfInfo.absoluteFilePath());
    const bool fromFedora = std::exchange(m_docsUseFedoraForNextCommentRefresh, false) && m_hasFedoraProjectionSnapshot;
    if (fromFedora) {
        QJsonArray canonical;
        const QString annotationPrefix = QStringLiteral("annotation\n%1\n").arg(pdfStem);
        for (auto it = m_fedoraProjections.constBegin(); it != m_fedoraProjections.constEnd(); ++it) {
            const auto collection = it.key().section(QLatin1Char('\n'), 1, 1);
            const auto *child = reading ? reading->forStem(collection) : nullptr;
            if ((!reading && !it.key().startsWith(annotationPrefix))
                || (reading && (!it.key().startsWith(QStringLiteral("annotation\n")) || !child)) || !it.value().isObject()) continue;
            QJsonObject record = it.value().toObject();
            if (child) record = Thoughts::offsetCommentPages(record, child->document.firstPage);
            const QString id = record.value(QStringLiteral("annotation_id")).toString(record.value(QStringLiteral("id")).toString());
            record.insert(QStringLiteral("id"), id);
            if (record.value(QStringLiteral("deleted")).toBool()) {
                suppressedIds.insert(id);
            } else {
                visible.append(record);
                canonical.append(record);
            }
        }
        QStringList orderedSuppressed(suppressedIds.begin(), suppressedIds.end());
        std::sort(orderedSuppressed.begin(), orderedSuppressed.end());
        for (const QString &id : std::as_const(orderedSuppressed)) {
            canonical.append(QJsonObject {{QStringLiteral("id"), id}, {QStringLiteral("deleted"), true}});
        }
        fingerprint = QCryptographicHash::hash(QJsonDocument(canonical).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex();
    } else if (reading) {
        QJsonArray canonical;
        for (const auto &child : reading->documents) {
            QByteArray ignored;
            QSet<QString> deleted;
            const auto records = DocsCommentSynchronizer::visibleSidecarComments(m_docsRenderRoot, child.stem(), &ignored, &error, &deleted);
            suppressedIds.unite(deleted);
            for (const auto &record : records) {
                const auto placed = Thoughts::offsetCommentPages(record, child.document.firstPage);
                visible.append(placed); canonical.append(placed);
            }
        }
        fingerprint = QCryptographicHash::hash(QJsonDocument(canonical).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex();
    } else if (QFileInfo(m_docsPreparedCommentLoad.value(QStringLiteral("pdf_path")).toString()).absoluteFilePath() == pdfInfo.absoluteFilePath()) {
        const QJsonObject prepared = std::exchange(m_docsPreparedCommentLoad, {});
        error = prepared.value(QStringLiteral("error")).toString();
        fingerprint = QByteArray::fromHex(prepared.value(QStringLiteral("fingerprint")).toString().toLatin1());
        for (const QJsonValue &value : prepared.value(QStringLiteral("visible")).toArray()) {
            if (value.isObject()) {
                visible.append(value.toObject());
            }
        }
        for (const QJsonValue &value : prepared.value(QStringLiteral("suppressed_ids")).toArray()) {
            if (value.isString()) {
                suppressedIds.insert(value.toString());
            }
        }
    } else {
        visible = DocsCommentSynchronizer::visibleSidecarComments(m_docsRenderRoot, pdfStem, &fingerprint, &error, &suppressedIds);
    }
    QMap<QString, QString> unplacedComments;
    QHash<QString, AnnotationSourceOrder> sidebarState;
    QJsonArray sourceConnectors;
    const QByteArray sourceFingerprint = fingerprint;
    if (m_docsOpenedPdfIdentityVerified) {
        auto publication = docsVerifiedCommentLocations(pdfInfo.fileName(), m_docsOpenedPdfSha256);
        if (reading) {
            for (const auto &child : reading->documents) {
                const auto local = docsVerifiedCommentLocations(child.filename, child.document.pdfSha256, QDir(QFileInfo(child.path).absolutePath()).filePath(child.document.packageSha256));
                for (auto it = local.locations.cbegin(); it != local.locations.cend(); ++it) {
                    auto locations = publication.locations.value(it.key());
                    for (const auto &location : it.value()) locations.append(Thoughts::offsetCommentPages(location.toObject(), child.document.firstPage));
                    publication.locations.insert(it.key(), locations);
                }
                for (const auto &segment : local.segments) {
                    auto shifted = segment.toObject();
                    shifted.insert(QStringLiteral("start_page"), shifted.value(QStringLiteral("start_page")).toInt() + child.document.firstPage);
                    publication.segments.append(shifted);
                }
            }
        }
        const auto &placements = publication.locations;
        QHash<QString, QJsonObject> logicalById;
        if (m_hasFedoraProjectionSnapshot) {
            const QString prefix = QStringLiteral("logical_comment\ncomments\n");
            for (auto it = m_fedoraProjections.constBegin(); it != m_fedoraProjections.constEnd(); ++it) {
                if (it.key().startsWith(prefix) && it.value().isObject()) {
                    const QJsonObject logical = it.value().toObject();
                    logicalById.insert(logical.value(QStringLiteral("comment_id")).toString(), logical);
                }
            }
        }
        sidebarState = Thoughts::commentSidebarState(logicalById, publication.segments);
        const auto anchorForRoot = [&logicalById](const QString &root) {
            return logicalById.value(root).value(QStringLiteral("scope")).toObject().value(QStringLiteral("anchor_id")).toString(root);
        };
        const auto effectiveOrigin = [&](const QJsonObject &payload) {
            const auto placement = payload.value(QStringLiteral("pdf_placement")).toObject();
            if (reading ? reading->forOrigin(placement) != nullptr :
                (placement.value(QStringLiteral("pdf_sha256")).toString() == m_docsOpenedPdfSha256 &&
                 placement.value(QStringLiteral("collection")).toString() == pdfStem)) return placement;
            return payload.value(QStringLiteral("origin")).toObject();
        };
        QSet<QString> present;
        for (const QJsonObject &record : std::as_const(visible)) present.insert(record.value(QStringLiteral("id")).toString());
        for (auto it = logicalById.cbegin(); it != logicalById.cend(); ++it) {
            const QString root = it->value(QStringLiteral("thread_root_id")).toString(it.key());
            const QJsonObject origin = effectiveOrigin(logicalById.value(root, it.value()));
            const bool originatedHere = reading ? reading->forOrigin(origin) != nullptr : origin.value(QStringLiteral("collection")).toString() == pdfStem;
            if (!present.contains(it.key()) && (placements.contains(anchorForRoot(root)) || originatedHere) && !it->value(QStringLiteral("deleted")).toBool()) {
                visible.append(QJsonObject {{QStringLiteral("id"), it.key()}, {QStringLiteral("subtype"), it->value(QStringLiteral("subtype"))}});
            }
        }
        QMap<QString, QStringList> rootsAtPoint;
        for (const QJsonObject &record : std::as_const(visible)) {
            const QString id = record.value(QStringLiteral("id")).toString();
            const QString root = logicalById.value(id).value(QStringLiteral("thread_root_id")).toString(id);
            if (root != id || logicalById.value(id).value(QStringLiteral("deleted")).toBool()) continue;
            for (const QJsonValue &value : placements.value(anchorForRoot(root))) {
                const QJsonObject location = value.toObject();
                const QJsonArray point = location.value(QStringLiteral("point")).toArray();
                if (point.size() != 2) continue;
                const QString key = QStringLiteral("%1:%2:%3").arg(location.value(QStringLiteral("page")).toInt())
                    .arg(qRound64(point.at(0).toDouble() * 10000)).arg(qRound64(point.at(1).toDouble() * 10000));
                if (!rootsAtPoint[key].contains(root)) rootsAtPoint[key].append(root);
            }
        }
        for (auto it = rootsAtPoint.begin(); it != rootsAtPoint.end(); ++it) std::sort(it->begin(), it->end());
        QList<QJsonObject> resolved;
        for (QJsonObject record : std::as_const(visible)) {
            const QString id = record.value(QStringLiteral("id")).toString();
            QJsonObject logical = logicalById.value(id);
            if (logical.isEmpty()) {
                // A placement cannot supply a missing canonical identity.
                // Keep a freshly created native annotation until its journal is delivered.
                if (m_docsNewCommentIds.contains(id)) resolved.append(record);
                else suppressedIds.insert(id);
                continue;
            }
            const QString rootId = logical.value(QStringLiteral("thread_root_id")).toString(id);
            if (logical.value(QStringLiteral("deleted")).toBool() || logicalById.value(rootId).value(QStringLiteral("deleted")).toBool()) {
                suppressedIds.insert(id);
                continue;
            }
            if (!logical.isEmpty()) {
                for (const QString &field : {QStringLiteral("contents"), QStringLiteral("author"), QStringLiteral("color"), QStringLiteral("priority"), QStringLiteral("location"), QStringLiteral("chatgpt_url"), QStringLiteral("thread_root_id")}) {
                    if (logical.contains(field)) record.insert(field, logical.value(field));
                }
                record.insert(QStringLiteral("created"), logical.value(QStringLiteral("created_at")));
                record.insert(QStringLiteral("modified"), logical.value(QStringLiteral("updated_at")));
                record.insert(QStringLiteral("in_reply_to"), logical.value(QStringLiteral("parent_id")));
            }
            const QJsonArray locations = placements.value(anchorForRoot(rootId));
            QMap<int, QJsonArray> quadsByPage;
            QJsonArray fragments;
            for (const QJsonValue &locationValue : locations) {
                const QJsonObject location = locationValue.toObject();
                const int pageNumber = location.value(QStringLiteral("page")).toInt() - 1;
                if (pageNumber < 0 || pageNumber >= static_cast<int>(m_document->pages())) continue;
                const auto *page = m_document->page(pageNumber);
                const QSizeF dpi = m_docsDocumentDpi.isValid() ? m_docsDocumentDpi : QSizeF(72.0, 72.0);
                const double width = page->width() * 72.0 / dpi.width();
                const double height = page->height() * 72.0 / dpi.height();
                const QJsonArray point = location.value(QStringLiteral("point")).toArray();
                if (point.size() == 2) {
                    const double sourceX = point.at(0).toDouble(), sourceY = point.at(1).toDouble();
                    const QString pointKey = QStringLiteral("%1:%2:%3").arg(pageNumber + 1).arg(qRound64(sourceX * 10000)).arg(qRound64(sourceY * 10000));
                    const int slot = std::max(0, int(rootsAtPoint.value(pointKey).indexOf(rootId)));
                    const int directionX = sourceX <= 0.5 ? 1 : -1, directionY = sourceY <= 0.5 ? 1 : -1;
                    const int columns = std::max(0, int((directionX > 0 ? 0.988 - sourceX : sourceX - 0.012) / 0.032)) + 1;
                    const double displayX = std::clamp(sourceX + directionX * (slot % columns) * 0.032, 0.0, std::max(0.0, 1.0 - 18.0 / width));
                    const double displayY = std::clamp(sourceY + directionY * (slot / columns) * 0.032, 0.0, std::max(0.0, 1.0 - 18.0 / height));
                    if (slot > 0 && id == rootId) sourceConnectors.append(QJsonObject {
                        {QStringLiteral("page"), pageNumber}, {QStringLiteral("source"), point}, {QStringLiteral("display"), QJsonArray {displayX, displayY}}});
                    const double x = displayX * width;
                    const double y = (1.0 - displayY) * height;
                    fragments.append(QJsonObject {{QStringLiteral("page"), pageNumber + 1}, {QStringLiteral("subtype"), QStringLiteral("Text")},
                                                   {QStringLiteral("rect"), QJsonArray {x, y - 18.0, x + 18.0, y}}});
                } else {
                    for (const QJsonValue &regionValue : location.value(QStringLiteral("regions")).toArray()) {
                        const QJsonArray region = regionValue.toArray();
                        const double left = region.at(0).toDouble() * width;
                        const double top = (1.0 - region.at(1).toDouble()) * height;
                        const double right = region.at(2).toDouble() * width;
                        const double bottom = (1.0 - region.at(3).toDouble()) * height;
                        for (const double number : {left, top, right, top, left, bottom, right, bottom}) quadsByPage[pageNumber].append(number);
                    }
                }
            }
            for (auto it = quadsByPage.cbegin(); it != quadsByPage.cend(); ++it) {
                double left = std::numeric_limits<double>::max(), bottom = left, right = 0.0, top = 0.0;
                for (qsizetype i = 0; i + 1 < it->size(); i += 2) {
                    left = std::min(left, it->at(i).toDouble()); right = std::max(right, it->at(i).toDouble());
                    bottom = std::min(bottom, it->at(i + 1).toDouble()); top = std::max(top, it->at(i + 1).toDouble());
                }
                QString subtype = record.value(QStringLiteral("subtype")).toString();
                if (subtype.isEmpty() || subtype == QLatin1String("Text")) subtype = QStringLiteral("Highlight");
                fragments.append(QJsonObject {{QStringLiteral("page"), it.key() + 1}, {QStringLiteral("subtype"), subtype},
                                               {QStringLiteral("rect"), QJsonArray {left, bottom, right, top}}, {QStringLiteral("quad_points"), it.value()}});
            }
            if (fragments.isEmpty()) {
                const QJsonObject origin = effectiveOrigin(logicalById.value(rootId, logical));
                const auto unplaced = [&] {
                    suppressedIds.insert(id);
                    if (id == rootId) unplacedComments.insert(id, record.value(QStringLiteral("contents")).toString());
                };
                const auto *originChild = reading ? reading->forOrigin(origin) : nullptr;
                if (reading ? !originChild : origin.value(QStringLiteral("pdf_sha256")).toString() != m_docsOpenedPdfSha256) { unplaced(); continue; }
                const int pageOffset = originChild ? originChild->document.firstPage : 0;
                QJsonArray captured = origin.value(QStringLiteral("fragments")).toArray();
                const bool rawOrigin = origin.value(QStringLiteral("geometry_format")).toString() == QLatin1String("pdf_points_bottom_left");
                if (rawOrigin) {
                    const QJsonObject raw = origin.value(QStringLiteral("raw_pdf_geometry")).toObject();
                    captured = raw.value(QStringLiteral("fragments")).toArray();
                    if (captured.isEmpty()) captured.append(raw);
                }
                if (captured.isEmpty()) captured.append(QJsonObject {{QStringLiteral("page_index"), origin.value(QStringLiteral("page_index"))},
                    {QStringLiteral("rect"), origin.value(QStringLiteral("rect"))}});
                bool validOrigin = true;
                for (const QJsonValue &value : captured) {
                    QJsonObject fragment = value.toObject();
                    const int localPage = rawOrigin ? fragment.value(QStringLiteral("page")).toInt(0) - 1 : fragment.value(QStringLiteral("page_index")).toInt(-1);
                    const int pageNumber = localPage + pageOffset;
                    if (originChild && (localPage < 0 || localPage >= originChild->document.pages)) { validOrigin = false; break; }
                    QJsonArray rectangle = fragment.value(QStringLiteral("rect")).toArray();
                    if (rectangle.size() != 4 || pageNumber < 0 || pageNumber >= static_cast<int>(m_document->pages())) { validOrigin = false; break; }
                    const auto *page = m_document->page(pageNumber);
                    const QSizeF dpi = m_docsDocumentDpi.isValid() ? m_docsDocumentDpi : QSizeF(72.0, 72.0);
                    const double width = page->width() * 72.0 / dpi.width(), height = page->height() * 72.0 / dpi.height();
                    if (rawOrigin) {
                        const double x1 = rectangle[0].toDouble(-1), x2 = rectangle[2].toDouble(-1);
                        const double y1 = rectangle[1].toDouble(-1), y2 = rectangle[3].toDouble(-1);
                        rectangle = QJsonArray {std::min(x1,x2)/width, 1.0-std::max(y1,y2)/height, std::max(x1,x2)/width, 1.0-std::min(y1,y2)/height};
                    }
                    if (!Thoughts::validNormalizedReviewCoordinates(rectangle, 4)) { validOrigin = false; break; }
                    fragment.insert(QStringLiteral("page"), pageNumber + 1);
                    fragment.insert(QStringLiteral("subtype"), record.value(QStringLiteral("subtype")));
                    fragment.insert(QStringLiteral("rect"), QJsonArray {rectangle[0].toDouble()*width, (1.0-rectangle[3].toDouble())*height,
                        rectangle[2].toDouble()*width, (1.0-rectangle[1].toDouble())*height});
                    const QJsonArray originalQuads = fragment.value(QStringLiteral("quad_points")).toArray();
                    QJsonArray converted;
                    if (originalQuads.size() % 8 != 0) { validOrigin = false; break; }
                    for (qsizetype i=0; i<originalQuads.size(); ++i) {
                        double n = originalQuads[i].toDouble(-1.0);
                        if (rawOrigin) n = i % 2 == 0 ? n/width : 1.0-n/height;
                        if (!std::isfinite(n) || n < 0 || n > 1) { validOrigin = false; break; }
                        converted.append(i % 2 == 0 ? n*width : (1.0-n)*height);
                    }
                    fragment.insert(QStringLiteral("quad_points"), converted);
                    fragments.append(fragment);
                }
                if (!validOrigin || fragments.isEmpty()) { unplaced(); continue; }

            }
            record.remove(QStringLiteral("quad_points"));
            const QJsonObject first = fragments.first().toObject();
            for (auto it = first.begin(); it != first.end(); ++it) record.insert(it.key(), it.value());
            record.insert(QStringLiteral("fragments"), fragments);
            record.insert(QStringLiteral("annotation_pdf_sha256"), m_docsOpenedPdfSha256);
            resolved.append(record);
        }
        visible = std::move(resolved);
        // Include resolved geometry so unchanged annotation revisions cannot
        // suppress a position update after a new PDF/source artifact arrives.
        QJsonArray rendered;
        for (const QJsonObject &record : std::as_const(visible)) {
            rendered.append(record);
        }
        const QByteArray renderedContents = fingerprint + QJsonDocument(rendered).toJson(QJsonDocument::Compact);
        fingerprint = QCryptographicHash::hash(renderedContents, QCryptographicHash::Sha256).toHex();
    }
    if (m_reviewsWidget) {
        m_reviewsWidget->setCommentSidebarState(sidebarState);
        m_reviewsWidget->setUnplacedComments(unplacedComments);
    }
    if (m_pageView) m_pageView->setCommentSourceConnectors(sourceConnectors);
    configureDocsCommentWatcher(QUrl::fromLocalFile(m_docsCommentPdfPath));
    m_docsCommentRefreshTimer->stop();
    const bool sourceMirrorsAlreadyReconciled = fromFedora || (!m_docsSourceReconciledCommentFingerprint.isEmpty() && sourceFingerprint == m_docsSourceReconciledCommentFingerprint);
    if (!error.isEmpty() || fingerprint == m_docsCommentFingerprint) {
        m_docsSourceReconciledCommentFingerprint.clear();
        m_docsCommentRefreshQueued = false;
        if (!m_pendingReviewAnnotationId.isEmpty() && error.isEmpty()) {
            const QString requested = std::exchange(m_pendingReviewAnnotationId, {});
            openReviewAnnotation(requested, m_pendingReviewAnnotationPage);
        }
        return;
    }
    QHash<QString, QJsonObject> desired;
    QHash<QString, QByteArray> desiredFingerprints;
    for (const QJsonObject &record : visible) {
        const QString id = record.value(QStringLiteral("id")).toString();
        bool valid = !id.isEmpty();
        for (const QJsonValue &physicalValue : docsPhysicalCommentsFromSidecar(record)) {
            const int page = physicalValue.toObject().value(QStringLiteral("page")).toInt() - 1;
            valid = valid && page >= 0 && page < static_cast<int>(m_document->pages());
        }
        if (!valid) {
            continue;
        }
        desired.insert(id, record);
        desiredFingerprints.insert(id, docsCommentRenderFingerprint(record));
    }
    m_docsCommentRecords = desired;

    QHash<QString, QPair<int, Okular::Annotation *>> existing = m_docsProjectedAnnotations;
    for (auto it = existing.begin(); it != existing.end();) {
        const int pageNumber = it->first;
        if (pageNumber < 0 || pageNumber >= static_cast<int>(m_document->pages()) || !m_document->page(pageNumber)->annotations().contains(it->second)) {
            it = existing.erase(it);
        } else {
            ++it;
        }
    }

    const auto existingIdsForLogicalComment = [&existing](const QString &logicalId) {
        QStringList ids;
        for (auto it = existing.cbegin(); it != existing.cend(); ++it) {
            if (it.key() == logicalId) {
                ids.append(it.key());
                continue;
            }
            const std::optional<Thoughts::CommentFragmentId> parsed = Thoughts::parseCommentFragmentId(it.key());
            if (parsed && parsed->logicalId == logicalId) {
                ids.append(it.key());
            }
        }
        std::sort(ids.begin(), ids.end(), [](const QString &left, const QString &right) {
            // Remove continuations before the root so the annotation model
            // never exposes a transient orphan row.
            return Thoughts::parseCommentFragmentId(left).has_value() && !Thoughts::parseCommentFragmentId(right).has_value();
        });
        return ids;
    };
    const auto removeExistingLogicalComment = [this, &existing, &existingIdsForLogicalComment](const QString &logicalId) {
        bool removedAll = true;
        for (const QString &physicalId : existingIdsForLogicalComment(logicalId)) {
            const auto found = existing.constFind(physicalId);
            if (found == existing.cend()) {
                continue;
            }
            if (!m_document->removePageAnnotationFromExternalSource(found->first, found->second)) {
                removedAll = false;
                continue;
            }
            existing.remove(physicalId);
        }
        return removedAll;
    };
    const auto hasEveryPhysicalComment = [&existing](const QJsonObject &record) {
        for (const QJsonValue &value : docsPhysicalCommentsFromSidecar(record)) {
            if (!existing.contains(value.toObject().value(QStringLiteral("id")).toString())) {
                return false;
            }
        }
        return true;
    };

    QScopedValueRollback<bool> applying(m_applyingDocsSidecar, true);
    QHash<QString, QByteArray> applied = m_docsAppliedCommentFingerprints;
    QStringList orderedSuppressedIds(suppressedIds.begin(), suppressedIds.end());
    std::sort(orderedSuppressedIds.begin(), orderedSuppressedIds.end());
    for (const QString &id : std::as_const(orderedSuppressedIds)) {
        if (removeExistingLogicalComment(id)) {
            applied.remove(id);
        }
    }
    const QStringList previouslyApplied = applied.keys();
    for (const QString &id : previouslyApplied) {
        if (desired.contains(id)) {
            continue;
        }
        if (removeExistingLogicalComment(id)) {
            applied.remove(id);
        }
    }

    for (auto it = desired.cbegin(); it != desired.cend(); ++it) {
        const QString id = it.key();
        const QByteArray renderFingerprint = desiredFingerprints.value(id);
        if (applied.value(id) == renderFingerprint && hasEveryPhysicalComment(it.value())) {
            continue;
        }
        if (!removeExistingLogicalComment(id)) {
            continue;
        }
        bool addedAll = true;
        for (const QJsonValue &physicalValue : docsPhysicalCommentsFromSidecar(it.value())) {
            const QJsonObject physical = physicalValue.toObject();
            const int pageNumber = physical.value(QStringLiteral("page")).toInt() - 1;
            const Okular::Page *page = m_document->page(pageNumber);
            const QSizeF dpi = m_docsDocumentDpi.isValid() ? m_docsDocumentDpi : QSizeF(72.0, 72.0);
            const double pageWidth = page->width() * 72.0 / dpi.width();
            const double pageHeight = page->height() * 72.0 / dpi.height();
            Okular::Annotation *annotation = docsAnnotationFromSidecar(physical, pageWidth, pageHeight);
            if (!annotation) {
                addedAll = false;
                break;
            }
            m_document->addPageAnnotationFromExternalSource(pageNumber, annotation);
            existing.insert(annotation->uniqueName(), qMakePair(pageNumber, annotation));
        }
        if (addedAll) {
            applied.insert(id, renderFingerprint);
        } else {
            removeExistingLogicalComment(id);
        }
    }
    connectDocsReplyThreads(desired, existing);
    if (m_reviewsWidget) m_reviewsWidget->refreshCommentThreads();
    m_docsProjectedAnnotations = existing;
    m_docsAppliedCommentFingerprints = applied;
    m_docsCommentFingerprint = fingerprint;
    m_docsSourceReconciledCommentFingerprint.clear();
    m_docsCommentRefreshQueued = false;
    // Companion edits update the canonical sidecar first. Repair the readable
    // TeX mirror from that authoritative record without waiting for another
    // PDF save or a source-file event.
    if (!sourceMirrorsAlreadyReconciled) {
        scheduleDocsSourceReconciliation(true);
    }
    if (m_pageView) {
        m_pageView->refreshOpenAnnotationRelatedInfo();
    }
    if (!m_pendingReviewAnnotationId.isEmpty()) {
        const QString requested = std::exchange(m_pendingReviewAnnotationId, {});
        openReviewAnnotation(requested, m_pendingReviewAnnotationPage);
    }
    qInfo() << "managed-review-projection active=" << desired.size() << "physical=" << existing.size() << "elapsed-ms=" << projectionTimer.elapsed();
}

void Part::clearDocsSourceWatcher()
{
    if (m_docsSourceReconcileTimer) {
        m_docsSourceReconcileTimer->stop();
    }
    m_docsSourcePdfPath.clear();
    m_docsSourceFingerprints.clear();
    m_docsSourceReconcileForce = false;
    m_docsSourceReconcileQueued = false;
    if (!m_docsSourceReconcileRunning) {
        m_docsSourceReconcileDocumentUrl.clear();
    }
}

void Part::configureDocsSourceWatcher(const QUrl &documentUrl, bool reconcileOnOpen)
{
    if (!shouldSyncDocsComments(documentUrl)) {
        clearDocsSourceWatcher();
        if (m_reviewsWidget && documentUrl.isValid()) {
            m_reviewsWidget->setDocsSourceLinkStatus(Reviews::DocsSourceLinkState::NotLinked);
        }
        return;
    }

    const QString pdfPath = QFileInfo(documentUrl.toLocalFile()).absoluteFilePath();
    if (m_docsSourcePdfPath != pdfPath) {
        clearDocsSourceWatcher();
        m_docsSourcePdfPath = pdfPath;
        if (m_reviewsWidget) {
            m_reviewsWidget->setDocsSourceLinkStatus(Reviews::DocsSourceLinkState::NoSource);
        }
    }
    scheduleDocsSourceReconciliation(reconcileOnOpen);
}

void Part::applyDocsSourceWatchPaths(const QUrl &documentUrl, const QStringList &sourceFiles, bool replaceExisting, bool reconcileOnOpen, bool refreshFingerprints)
{
    if (!shouldSyncDocsComments(documentUrl)) {
        clearDocsSourceWatcher();
        return;
    }

    const QString pdfPath = QFileInfo(documentUrl.toLocalFile()).absoluteFilePath();
    const bool changedDocument = m_docsSourcePdfPath != pdfPath;
    if (changedDocument) {
        clearDocsSourceWatcher();
        m_docsSourcePdfPath = pdfPath;
        replaceExisting = true;
    }

    QSet<QString> desiredFiles;
    if (!replaceExisting) {
        const QStringList existingFiles = m_docsSourceFingerprints.keys();
        desiredFiles = QSet<QString>(existingFiles.begin(), existingFiles.end());
    }
    for (const QString &path : sourceFiles) {
        if (!path.isEmpty()) {
            desiredFiles.insert(QFileInfo(path).absoluteFilePath());
        }
    }
    if (m_reviewsWidget) {
        if (desiredFiles.isEmpty()) {
            m_reviewsWidget->setDocsSourceLinkStatus(Reviews::DocsSourceLinkState::NoSource);
        } else {
            QStringList displayedFiles(desiredFiles.begin(), desiredFiles.end());
            std::sort(displayedFiles.begin(), displayedFiles.end());
            m_reviewsWidget->setDocsSourceLinkStatus(Reviews::DocsSourceLinkState::Linked, displayedFiles.size(), docsSourceProjects(m_docsRenderRoot, displayedFiles));
        }
    }
    const QStringList knownFiles = m_docsSourceFingerprints.keys();
    for (const QString &path : knownFiles) {
        if (!desiredFiles.contains(path)) {
            m_docsSourceFingerprints.remove(path);
        }
    }
    for (const QString &path : desiredFiles) {
        if (!m_docsSourceFingerprints.contains(path)) {
            m_docsSourceFingerprints.insert(path, QByteArrayLiteral("unknown"));
        }
    }
    Q_UNUSED(refreshFingerprints)

    if (changedDocument || reconcileOnOpen) {
        scheduleDocsSourceReconciliation(true);
    } else if (m_docsSourceReconcileQueued) {
        scheduleDocsSourceReconciliation();
    }
}

void Part::followDocsChanges()
{
    // Sidecars and sources of the open document can be rewritten by editors,
    // agents and thoughtsctl. React to the shared change stream for the
    // current docs root instead of re-hashing every source on a timer.
    Thoughts::DocsChangeMonitor *monitor = Thoughts::DocsChangeMonitor::forRoot(m_docsRenderRoot);
    if (monitor == m_docsChangeMonitor) {
        return;
    }
    if (m_docsChangeMonitor) {
        disconnect(m_docsChangeMonitor, nullptr, this, nullptr);
    }
    m_docsChangeMonitor = monitor;
    connect(monitor, &Thoughts::DocsChangeMonitor::pathsChanged, this, &Part::handleDocsPathsChanged);
    connect(monitor, &Thoughts::DocsChangeMonitor::rescanRequired, this, [this] {
        if (!m_docsCompanionActive) {
            return;
        }
        if (!m_hasFedoraProjectionSnapshot) {
            scheduleDocsCommentRefresh();
        }
        scheduleDocsSourceReconciliation();
    });
}

void Part::handleDocsPathsChanged(const QStringList &paths)
{
    if (!m_docsCompanionActive) {
        return;
    }
    const QString root = QDir::cleanPath(m_docsRenderRoot);
    const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
    const QString commentsDirectory = root + QStringLiteral("/comments");
    bool comments = false;
    bool sources = false;
    QSet<QString> canonicalSources;
    bool canonicalSourcesKnown = false;
    for (const QString &path : paths) {
        comments = comments || Thoughts::DocsChangeMonitor::isWithin(path, commentsDirectory);
        if (!sources && m_docsSourceFingerprints.contains(path)) {
            sources = true;
        } else if (!sources && !m_docsSourceFingerprints.isEmpty()) {
            // Reported paths use the configured root, while sources may be
            // listed through a symbolic link or below the resolved root.
            if (!canonicalSourcesKnown) {
                for (auto it = m_docsSourceFingerprints.cbegin(); it != m_docsSourceFingerprints.cend(); ++it) {
                    const QString canonical = QFileInfo(it.key()).canonicalFilePath();
                    if (!canonical.isEmpty()) {
                        canonicalSources.insert(canonical);
                    }
                }
                canonicalSourcesKnown = true;
            }
            const bool rootRelative = !canonicalRoot.isEmpty() && canonicalRoot != root && Thoughts::DocsChangeMonitor::isWithin(path, root);
            const QString resolved = rootRelative ? canonicalRoot + path.mid(root.size()) : path;
            sources = canonicalSources.contains(path) || m_docsSourceFingerprints.contains(resolved) || canonicalSources.contains(resolved);
        }
    }
    if (comments && !m_hasFedoraProjectionSnapshot) {
        scheduleDocsCommentRefresh();
    }
    if (sources) {
        scheduleDocsSourceReconciliation();
    }
}

void Part::scheduleDocsSourceReconciliation(bool force)
{
    if (m_docsSourcePdfPath.isEmpty() || !m_docsSourceReconcileTimer) {
        return;
    }
    m_docsSourceReconcileForce = m_docsSourceReconcileForce || force;
    m_docsSourceReconcileTimer->start();
}

void Part::reconcileDocsSourceChanges()
{
    if (m_docsSourcePdfPath.isEmpty() || !m_document->isOpened()) {
        return;
    }
    const QUrl documentUrl = m_document->currentDocument();
    if (QFileInfo(documentUrl.toLocalFile()).absoluteFilePath() != m_docsSourcePdfPath) {
        return;
    }
    if (m_docsSourceReconcileRunning || m_syncingDocsComments || isModified()) {
        m_docsSourceReconcileQueued = true;
        return;
    }

    const bool force = m_docsSourceReconcileForce;
    m_docsSourceReconcileForce = false;
    m_docsSourceReconcileQueued = false;
    m_docsSourceReconcileRunning = true;
    m_docsSourceReconcileDocumentUrl = documentUrl;

    const QString docsRoot = m_docsRenderRoot;
    const QString synctexExecutable = m_docsSyncTexExecutable;
    const QProcessEnvironment environment = docsToolEnvironment();
    const int synctexTimeoutMs = m_docsSyncTexTimeoutMs;
    const QHash<QString, QByteArray> expectedFingerprints = m_docsSourceFingerprints;
    const QStringList sourcePaths = expectedFingerprints.keys();
    QPromise<QJsonObject> promise;
    const QFuture<QJsonObject> future = promise.future();
    promise.start();
    m_docsSourceReconcileWatcher->setFuture(future);
    QThreadPool::globalInstance()->start([promise = std::move(promise), docsRoot, synctexExecutable, environment, synctexTimeoutMs, documentUrl, sourcePaths, expectedFingerprints, force]() mutable {
        QHash<QString, QByteArray> currentFingerprints;
        bool changed = force;
        for (const QString &path : sourcePaths) {
            const QByteArray fingerprint = docsSourceFingerprint(path);
            currentFingerprints.insert(path, fingerprint);
            changed = changed || fingerprint != expectedFingerprints.value(path);
        }
        QJsonObject sourceFingerprints;
        for (auto iterator = currentFingerprints.cbegin(); iterator != currentFingerprints.cend(); ++iterator) {
            sourceFingerprints.insert(iterator.key(), QString::fromLatin1(iterator.value().toHex()));
        }
        if (!changed) {
            promise.addResult(QJsonObject {
                {QStringLiteral("success"), true},
                {QStringLiteral("skipped"), true},
                {QStringLiteral("source_fingerprints"), sourceFingerprints},
            });
            promise.finish();
            return;
        }

        DocsCommentSynchronizer synchronizer({
            .docsRoot = docsRoot,
            .synctexExecutable = synctexExecutable,
            .processEnvironment = environment,
            .synctexTimeoutMs = synctexTimeoutMs,
            .lockTimeoutMs = 200,
        });
        DocsCommentSynchronizer::Result result;
        if (const auto reading = Thoughts::ProjectReadingContext::load(documentUrl.toLocalFile())) {
            result.success = true;
            result.watchedSourceFilesComplete = true;
            for (const auto &child : reading->documents) {
                const auto local = synchronizer.reconcileSourceAnchorChanges(QUrl::fromLocalFile(QDir(docsRoot).filePath(QStringLiteral("current_pdfs/") + child.filename)));
                result.success = result.success && local.success;
                if (!local.success) result.failureReason += local.failureReason + QLatin1Char('\n');
                result.logLines.append(local.logLines);
                result.watchedSourceFiles.append(local.watchedSourceFiles);
                result.watchedSourceFilesComplete = result.watchedSourceFilesComplete && local.watchedSourceFilesComplete;
                result.sourceAnchorDeletions += local.sourceAnchorDeletions;
                result.sourceAnchorsOrphaned += local.sourceAnchorsOrphaned;
                result.sourceCommentEditsImported += local.sourceCommentEditsImported;
                result.anchorMirrorsUpdated += local.anchorMirrorsUpdated;
                result.totalMs += local.totalMs;
            }
            result.watchedSourceFiles.removeDuplicates();
        } else result = synchronizer.reconcileSourceAnchorChanges(documentUrl);
        QJsonArray logLines;
        for (const QString &line : result.logLines) {
            logLines.append(line);
        }
        sourceFingerprints = {};
        for (const QString &path : sourcePaths) {
            sourceFingerprints.insert(path, QString::fromLatin1(docsSourceFingerprint(path).toHex()));
        }
        QJsonArray watchedSourceFiles;
        for (const QString &path : result.watchedSourceFiles) {
            watchedSourceFiles.append(path);
        }
        promise.addResult(QJsonObject {
            {QStringLiteral("success"), result.success},
            {QStringLiteral("failure_reason"), result.failureReason},
            {QStringLiteral("source_anchor_deletions"), result.sourceAnchorDeletions},
            {QStringLiteral("source_anchors_orphaned"), result.sourceAnchorsOrphaned},
            {QStringLiteral("source_comment_edits_imported"), result.sourceCommentEditsImported},
            {QStringLiteral("anchor_mirrors_updated"), result.anchorMirrorsUpdated},
            {QStringLiteral("total_ms"), result.totalMs},
            {QStringLiteral("log_lines"), logLines},
            {QStringLiteral("source_fingerprints"), sourceFingerprints},
            {QStringLiteral("watched_source_files"), watchedSourceFiles},
            {QStringLiteral("watched_source_files_complete"), result.watchedSourceFilesComplete},
            {QStringLiteral("comment_fingerprint"), QString::fromLatin1(result.visibleSidecarFingerprint.toHex())},
        });
        promise.finish();
    });
}

void Part::finishDocsSourceReconciliation()
{
    const QJsonObject outcome = m_docsSourceReconcileWatcher->result();
    const QUrl reconciledUrl = m_docsSourceReconcileDocumentUrl;
    m_docsSourceReconcileRunning = false;
    m_docsSourceReconcileDocumentUrl.clear();

    const bool sameDocument = m_document->isOpened() && m_document->currentDocument() == reconciledUrl && QFileInfo(reconciledUrl.toLocalFile()).absoluteFilePath() == m_docsSourcePdfPath;
    if (sameDocument) {
        const QJsonObject sourceFingerprints = outcome.value(QStringLiteral("source_fingerprints")).toObject();
        for (auto iterator = sourceFingerprints.begin(); iterator != sourceFingerprints.end(); ++iterator) {
            if (m_docsSourceFingerprints.contains(iterator.key()) && iterator.value().isString()) {
                m_docsSourceFingerprints.insert(iterator.key(), QByteArray::fromHex(iterator.value().toString().toLatin1()));
            }
        }
    }
    if (outcome.value(QStringLiteral("skipped")).toBool()) {
        if (sameDocument && m_docsSourceReconcileQueued) {
            m_docsSourceReconcileTimer->start(250);
        }
        return;
    }

    const QString logDirectory = QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server/logs"));
    m_docsSourceSyncLogPath = QDir(logDirectory).filePath(QStringLiteral("thoughts-comment-source-sync.log"));
    QSaveFile logFile(m_docsSourceSyncLogPath);
    if (QDir().mkpath(logDirectory) && logFile.open(QIODevice::WriteOnly)) {
        QTextStream stream(&logFile);
        for (const QJsonValue &value : outcome.value(QStringLiteral("log_lines")).toArray()) {
            stream << value.toString() << '\n';
        }
        logFile.commit();
    }

    if (outcome.value(QStringLiteral("success")).toBool()) {
        if (sameDocument) {
            m_docsSourceReconciledCommentFingerprint = QByteArray::fromHex(outcome.value(QStringLiteral("comment_fingerprint")).toString().toLatin1());
            QStringList watchedSourceFiles;
            for (const QJsonValue &value : outcome.value(QStringLiteral("watched_source_files")).toArray()) {
                if (value.isString()) {
                    watchedSourceFiles.append(value.toString());
                }
            }
            applyDocsSourceWatchPaths(reconciledUrl, watchedSourceFiles, outcome.value(QStringLiteral("watched_source_files_complete")).toBool());
            scheduleDocsCommentRefresh();
        }
    } else {
        const QString failureReason = outcome.value(QStringLiteral("failure_reason")).toString();
        if (failureReason.contains(QStringLiteral("another synchronization"))) {
            m_docsSourceReconcileQueued = sameDocument;
        } else if (sameDocument) {
            errorMessage(i18n("Comment source synchronization failed (%1). Log: %2", failureReason, QDir::toNativeSeparators(m_docsSourceSyncLogPath)), 10000);
        }
    }

    if (sameDocument && m_docsSourceReconcileQueued) {
        m_docsSourceReconcileTimer->start(250);
    }
}

bool Part::shouldSyncDocsComments(const QUrl &documentUrl) const
{
    if (!documentUrl.isLocalFile()) {
        return false;
    }

    const QFileInfo documentInfo(documentUrl.toLocalFile());
    if (documentInfo.suffix() == QLatin1String("thoughts-project"))
        return Thoughts::ProjectReadingContext::load(documentInfo.absoluteFilePath()).has_value();
    // An earlier version of a Library PDF (opened from a card's History) is only for reading.
    if (Thoughts::PdfVersions::isCachedVersion(documentInfo.absoluteFilePath())) return false;
    if (documentInfo.suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) != 0) {
        return false;
    }

    return !Thoughts::DocumentIdentityStore(m_docsRenderRoot).forFile(documentInfo.absoluteFilePath()).isEmpty();
}

bool Part::isOutsidePdf(const QUrl &documentUrl) const
{
    if (!documentUrl.isLocalFile()) return false;
    const QFileInfo info(documentUrl.toLocalFile());
    return info.isFile() && info.suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0 && !Thoughts::PdfVersions::isCachedVersion(info.absoluteFilePath())
        && !shouldSyncDocsComments(documentUrl);
}

void Part::saveOutsidePdfComments(bool force)
{
    if (m_outsideCommentsSaveTimer) m_outsideCommentsSaveTimer->stop();
    if (m_outsideCommentsPdfPath.isEmpty() || !m_document->isOpened() || (!force && m_document->isHistoryClean())) return;
    QString error;
    if (!Thoughts::OutsidePdfComments::save(m_outsideCommentsPdfPath, m_outsideCommentsSha256, m_document->localAnnotationsXml(), &error)) {
        errorMessage(i18n("The comments could not be saved on this computer: %1", error), 15000);
        return;
    }
    // Saved: nothing to ask about when the PDF closes.
    m_document->setHistoryClean(true);
}

bool Part::isWorkspaceThirdPartyPdf(const QUrl &documentUrl) const
{
    if (!documentUrl.isLocalFile()) return false;

    const QFileInfo documentInfo(documentUrl.toLocalFile());
    if (!documentInfo.isFile() || documentInfo.suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) != 0) return false;
    if (Thoughts::PdfVersions::isCachedVersion(documentInfo.absoluteFilePath())) return false;
    static const QRegularExpression identifier(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]*$"));
    if (!identifier.match(documentInfo.completeBaseName()).hasMatch()) return false;

    const QFileInfo thirdPartyInfo(QDir(m_docsRenderRoot).filePath(QStringLiteral("third_party")));
    const QString thirdPartyPath = thirdPartyInfo.canonicalFilePath();
    return !thirdPartyPath.isEmpty() && QFileInfo(documentInfo.canonicalFilePath()).absolutePath() == thirdPartyPath;
}

QUrl Part::normalizedDocsLibraryUrl(const QUrl &documentUrl) const
{
    if (!documentUrl.isLocalFile()) {
        return documentUrl;
    }

    const QFileInfo documentInfo(documentUrl.toLocalFile());
    if (!documentInfo.isFile() || documentInfo.suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) != 0) {
        return documentUrl;
    }

    const QFileInfo parentInfo(documentInfo.absolutePath());
    const QFileInfo docsRootInfo(m_docsRenderRoot);
    static const QRegularExpression generationDirectoryPattern(QStringLiteral("^\\.thoughts-library-stage-[0-9a-f]{64}$"));
    if (QDir::cleanPath(parentInfo.absolutePath()) != QDir::cleanPath(docsRootInfo.absoluteFilePath()) || !generationDirectoryPattern.match(parentInfo.fileName()).hasMatch()) {
        return documentUrl;
    }

    const QString currentPath = QDir(m_docsRenderRoot).filePath(QStringLiteral("current_pdfs/%1").arg(documentInfo.fileName()));
    return QFileInfo(currentPath).isFile() ? QUrl::fromLocalFile(QFileInfo(currentPath).absoluteFilePath()) : documentUrl;
}

static QJsonValue nullableString(const QString &value)
{
    return value.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(value);
}

static QJsonValue pdfDateString(const QDateTime &date)
{
    if (!date.isValid()) {
        return QJsonValue(QJsonValue::Null);
    }

    const int offsetSeconds = date.offsetFromUtc();
    QString suffix;
    if (offsetSeconds == 0) {
        suffix = QStringLiteral("Z");
    } else {
        const int absoluteOffsetMinutes = qAbs(offsetSeconds) / 60;
        suffix = QStringLiteral("%1%2'%3'").arg(offsetSeconds < 0 ? QLatin1Char('-') : QLatin1Char('+')).arg(absoluteOffsetMinutes / 60, 2, 10, QLatin1Char('0')).arg(absoluteOffsetMinutes % 60, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("D:%1%2").arg(date.toString(QStringLiteral("yyyyMMddHHmmss")), suffix);
}

static int pdfAnnotationFlags(const Okular::Annotation *annotation)
{
    const int okularFlags = annotation->flags();
    int pdfFlags = 0;
    if (!(okularFlags & Okular::Annotation::DenyPrint)) {
        pdfFlags |= 4; // PDF Print
    }
    if (okularFlags & Okular::Annotation::Hidden) {
        pdfFlags |= 2; // PDF Hidden
    }
    if (okularFlags & Okular::Annotation::FixedSize) {
        pdfFlags |= 8; // PDF NoZoom
    }
    if (okularFlags & Okular::Annotation::FixedRotation) {
        pdfFlags |= 16; // PDF NoRotate
    }
    if (okularFlags & Okular::Annotation::DenyWrite) {
        pdfFlags |= 64; // PDF ReadOnly
    }
    if (okularFlags & Okular::Annotation::DenyDelete) {
        pdfFlags |= 128; // PDF Locked
    }
    if (okularFlags & Okular::Annotation::ToggleHidingOnMouse) {
        pdfFlags |= 256; // PDF ToggleNoView
    }
    return pdfFlags;
}

static QJsonArray normalizedRectJson(const Okular::NormalizedRect &rect)
{
    return {rect.left, rect.top, rect.right, rect.bottom};
}

static void appendNormalizedPoint(QJsonArray *points, const Okular::NormalizedPoint &point)
{
    points->append(point.x);
    points->append(point.y);
}

static QJsonObject docsSnapshotFragment(const QJsonObject &comment)
{
    QJsonObject fragment;
    for (const QString &field : {
             QStringLiteral("page"),
             QStringLiteral("page_width"),
             QStringLiteral("page_height"),
             QStringLiteral("page_rotation"),
             QStringLiteral("rect"),
             QStringLiteral("quad_points"),
         }) {
        fragment.insert(field, comment.value(field));
    }
    return fragment;
}

int Part::normalizeLegacyCrossPageComments()
{
    struct Candidate {
        int page = -1;
        Okular::HighlightAnnotation *annotation = nullptr;
    };
    QList<Candidate> candidates;
    QHash<Okular::Annotation *, int> pageByAnnotation;
    for (uint pageNumber = 0; pageNumber < m_document->pages(); ++pageNumber) {
        for (Okular::Annotation *annotation : m_document->page(pageNumber)->annotations()) {
            if (annotation->subType() != Okular::Annotation::AHighlight || annotation->uniqueName().isEmpty() || Thoughts::parseCommentFragmentId(annotation->uniqueName())) {
                continue;
            }
            candidates.append({
                static_cast<int>(pageNumber),
                static_cast<Okular::HighlightAnnotation *>(annotation),
            });
            pageByAnnotation.insert(annotation, static_cast<int>(pageNumber));
        }
    }

    const auto sameSignature = [](const Candidate &left, const Candidate &right) {
        const auto &leftStyle = left.annotation->style();
        const auto &rightStyle = right.annotation->style();
        return left.annotation->highlightType() == right.annotation->highlightType() && left.annotation->author() == right.annotation->author() && left.annotation->creationDate().isValid() &&
            left.annotation->creationDate() == right.annotation->creationDate() && left.annotation->flags() == right.annotation->flags() && leftStyle.color() == rightStyle.color() &&
            qFuzzyCompare(leftStyle.opacity(), rightStyle.opacity()) && qFuzzyCompare(leftStyle.width(), rightStyle.width()) && leftStyle.lineStyle() == rightStyle.lineStyle();
    };

    QHash<Okular::Annotation *, QList<Okular::Annotation *>> outgoing;
    QHash<Okular::Annotation *, QList<Okular::Annotation *>> incoming;
    for (const Candidate &source : std::as_const(candidates)) {
        if (source.annotation->boundingRectangle().bottom < 0.85) {
            continue;
        }
        for (const Candidate &destination : std::as_const(candidates)) {
            if (destination.page != source.page + 1 || !destination.annotation->contents().isEmpty() || destination.annotation->boundingRectangle().top > 0.15 || !sameSignature(source, destination)) {
                continue;
            }
            outgoing[source.annotation].append(destination.annotation);
            incoming[destination.annotation].append(source.annotation);
        }
    }

    int normalizedGroups = 0;
    QSet<Okular::Annotation *> claimed;
    for (const Candidate &root : std::as_const(candidates)) {
        if (root.annotation->contents().isEmpty() || root.annotation->boundingRectangle().bottom < 0.85 || claimed.contains(root.annotation)) {
            continue;
        }
        QList<Okular::Annotation *> chain {root.annotation};
        Okular::Annotation *cursor = root.annotation;
        bool ambiguous = false;
        while (!outgoing.value(cursor).isEmpty()) {
            const QList<Okular::Annotation *> nextCandidates = outgoing.value(cursor);
            if (nextCandidates.size() != 1) {
                ambiguous = true;
                break;
            }
            Okular::Annotation *next = nextCandidates.constFirst();
            if (incoming.value(next).size() != 1 || claimed.contains(next) || chain.contains(next)) {
                ambiguous = true;
                break;
            }
            chain.append(next);
            cursor = next;
        }
        if (ambiguous || chain.size() < 2) {
            continue;
        }
        bool canNormalize = true;
        for (qsizetype fragmentIndex = 1; fragmentIndex < chain.size(); ++fragmentIndex) {
            if (!m_document->canModifyPageAnnotation(chain.at(fragmentIndex))) {
                canNormalize = false;
                break;
            }
        }
        if (!canNormalize) {
            continue;
        }
        for (qsizetype fragmentIndex = 1; fragmentIndex < chain.size(); ++fragmentIndex) {
            Okular::Annotation *const continuation = chain.at(fragmentIndex);
            continuation->setUniqueName(Thoughts::makeCommentFragmentId(root.annotation->uniqueName(), static_cast<int>(fragmentIndex)));
            m_document->modifyPageAnnotationPropertiesFromExternalSource(pageByAnnotation.value(continuation, -1), continuation);
        }
        for (Okular::Annotation *annotation : std::as_const(chain)) {
            claimed.insert(annotation);
        }
        ++normalizedGroups;
    }
    return normalizedGroups;
}

bool Part::writeDocsCommentSnapshot(const QUrl &documentUrl, QJsonObject *snapshot, QString *failureReason, const QString &onlyLogicalId) const
{
    const auto fail = [failureReason](const QString &reason) {
        if (failureReason) {
            *failureReason = reason;
        }
        return false;
    };
    if (!snapshot) {
        return fail(i18n("the comment snapshot destination is unavailable"));
    }

    QList<QJsonObject> physicalComments;
    const QSizeF documentDpi = m_docsDocumentDpi.isValid() ? m_docsDocumentDpi : QSizeF(72.0, 72.0);
    for (uint pageNumber = 0; pageNumber < m_document->pages(); ++pageNumber) {
        const Okular::Page *page = m_document->page(pageNumber);
        // Okular's Poppler pages are sized in screen pixels at the generator's
        // document DPI, whereas PDF annotation Rect/QuadPoints and the JSONL
        // schema use 1/72-inch PDF points.
        const double pageWidthPoints = page->width() * 72.0 / documentDpi.width();
        const double pageHeightPoints = page->height() * 72.0 / documentDpi.height();
        for (const Okular::Annotation *annotation : page->annotations()) {
            QString annotationLogicalId = annotation->uniqueName();
            if (const std::optional<Thoughts::CommentFragmentId> fragment = Thoughts::parseCommentFragmentId(annotationLogicalId)) {
                annotationLogicalId = fragment->logicalId;
            }
            if (!onlyLogicalId.isEmpty()) {
                if (annotationLogicalId != onlyLogicalId) {
                    continue;
                }
            }
            QString subtype;
            QString selectedText;
            QJsonValue name(QJsonValue::Null);
            QJsonValue quadPoints(QJsonValue::Null);
            if (annotation->subType() == Okular::Annotation::AText) {
                const auto *textAnnotation = static_cast<const Okular::TextAnnotation *>(annotation);
                if (textAnnotation->textType() != Okular::TextAnnotation::Linked
                    || (annotation->contents().isEmpty() && !m_docsCommentRecords.contains(annotationLogicalId))) {
                    continue;
                }
                subtype = QStringLiteral("Text");
                name = nullableString(textAnnotation->textIcon());
            } else if (annotation->subType() == Okular::Annotation::AHighlight) {
                const auto *highlightAnnotation = static_cast<const Okular::HighlightAnnotation *>(annotation);
                switch (highlightAnnotation->highlightType()) {
                case Okular::HighlightAnnotation::Highlight:
                    subtype = QStringLiteral("Highlight");
                    break;
                case Okular::HighlightAnnotation::Underline:
                    subtype = QStringLiteral("Underline");
                    break;
                case Okular::HighlightAnnotation::Squiggly:
                    subtype = QStringLiteral("Squiggly");
                    break;
                case Okular::HighlightAnnotation::StrikeOut:
                    subtype = QStringLiteral("StrikeOut");
                    break;
                }

                QJsonArray points;
                Okular::RegularAreaRect textArea;
                for (const Okular::HighlightAnnotation::Quad &quad : highlightAnnotation->highlightQuads()) {
                    // PDF QuadPoints order is top-left, top-right,
                    // bottom-left, bottom-right. Okular stores the bottom
                    // edge in points 0 and 1.
                    appendNormalizedPoint(&points, quad.point(3));
                    appendNormalizedPoint(&points, quad.point(2));
                    appendNormalizedPoint(&points, quad.point(0));
                    appendNormalizedPoint(&points, quad.point(1));
                    double left = 1.0;
                    double top = 1.0;
                    double right = 0.0;
                    double bottom = 0.0;
                    for (int pointIndex = 0; pointIndex < 4; ++pointIndex) {
                        const Okular::NormalizedPoint point = quad.point(pointIndex);
                        left = std::min(left, point.x);
                        top = std::min(top, point.y);
                        right = std::max(right, point.x);
                        bottom = std::max(bottom, point.y);
                    }
                    textArea.appendShape(Okular::NormalizedRect(left, top, right, bottom));
                }
                if (points.isEmpty()) {
                    return fail(i18n("a text markup annotation on page %1 has no quadrilateral geometry", pageNumber + 1));
                }
                quadPoints = points;
                selectedText = page->text(&textArea, Okular::TextPage::CentralPixelTextAreaInclusionBehaviour);
            } else {
                continue;
            }

            QJsonValue color(QJsonValue::Null);
            if (annotation->style().color().isValid()) {
                const QColor annotationColor = annotation->style().color();
                color = QJsonArray {annotationColor.redF(), annotationColor.greenF(), annotationColor.blueF()};
            }

            QJsonObject comment {
                {QStringLiteral("page"), static_cast<int>(pageNumber + 1)},
                {QStringLiteral("page_width"), pageWidthPoints},
                {QStringLiteral("page_height"), pageHeightPoints},
                {QStringLiteral("page_rotation"), static_cast<int>(page->orientation())},
                {QStringLiteral("subtype"), subtype},
                {QStringLiteral("id"), nullableString(annotation->uniqueName())},
                {QStringLiteral("rect"), normalizedRectJson(annotation->boundingRectangle())},
                {QStringLiteral("quad_points"), quadPoints},
                {QStringLiteral("author"), nullableString(annotation->author())},
                {QStringLiteral("modified"), pdfDateString(annotation->modificationDate())},
                {QStringLiteral("created"), pdfDateString(annotation->creationDate())},
                {QStringLiteral("contents"), annotation->contents()},
                {QStringLiteral("selected_text"), selectedText},
                {QStringLiteral("color"), color},
                {QStringLiteral("flags"), pdfAnnotationFlags(annotation)},
                {QStringLiteral("opacity"), annotation->style().opacity()},
                {QStringLiteral("name"), name},
                {QStringLiteral("created_in_app"), nullableString(annotation->creatorApplication())},
                {QStringLiteral("annotation_pdf_sha256"), nullableString(m_docsOpenedPdfSha256)},
            };
            physicalComments.append(comment);
        }
    }

    QHash<QString, QList<int>> roots;
    for (qsizetype index = 0; index < physicalComments.size(); ++index) {
        const QString id = physicalComments.at(index).value(QStringLiteral("id")).toString();
        if (!id.isEmpty() && !Thoughts::parseCommentFragmentId(id)) {
            roots[id].append(static_cast<int>(index));
        }
    }

    struct IndexedFragment {
        int fragmentIndex = 0;
        int physicalIndex = -1;
    };
    QHash<int, QList<IndexedFragment>> fragmentsByRoot;
    for (qsizetype index = 0; index < physicalComments.size(); ++index) {
        const QString id = physicalComments.at(index).value(QStringLiteral("id")).toString();
        const std::optional<Thoughts::CommentFragmentId> parsed = Thoughts::parseCommentFragmentId(id);
        const QList<int> rootIndexes = parsed ? roots.value(parsed->logicalId) : QList<int>();
        if (!parsed || rootIndexes.size() != 1) {
            continue;
        }
        const int rootIndex = rootIndexes.constFirst();
        fragmentsByRoot[rootIndex].append({
            parsed->index,
            static_cast<int>(index),
        });
    }

    QHash<int, QList<IndexedFragment>> validFragmentsByRoot;
    QSet<int> continuationIndexes;
    for (auto fragmentsIt = fragmentsByRoot.cbegin(); fragmentsIt != fragmentsByRoot.cend(); ++fragmentsIt) {
        const int rootIndex = fragmentsIt.key();
        const QJsonObject root = physicalComments.at(rootIndex);
        const QString rootSubtype = root.value(QStringLiteral("subtype")).toString();
        const QString rootContents = root.value(QStringLiteral("contents")).toString();
        const int rootPage = root.value(QStringLiteral("page")).toInt();
        QList<IndexedFragment> continuations = fragmentsIt.value();
        std::sort(continuations.begin(), continuations.end(), [](const IndexedFragment &left, const IndexedFragment &right) { return left.fragmentIndex < right.fragmentIndex; });

        bool valid = rootSubtype != QLatin1String("Text");
        for (qsizetype position = 0; valid && position < continuations.size(); ++position) {
            const IndexedFragment &continuation = continuations.at(position);
            const QJsonObject fragment = physicalComments.at(continuation.physicalIndex);
            const QString fragmentContents = fragment.value(QStringLiteral("contents")).toString();
            valid = continuation.fragmentIndex == position + 1 && fragment.value(QStringLiteral("page")).toInt() == rootPage + position + 1 && fragment.value(QStringLiteral("subtype")).toString() == rootSubtype &&
                (fragmentContents.isEmpty() || fragmentContents == rootContents);
        }
        if (!valid) {
            continue;
        }
        validFragmentsByRoot.insert(rootIndex, continuations);
        for (const IndexedFragment &continuation : std::as_const(continuations)) {
            continuationIndexes.insert(continuation.physicalIndex);
        }
    }

    QJsonArray comments;
    for (qsizetype index = 0; index < physicalComments.size(); ++index) {
        if (continuationIndexes.contains(static_cast<int>(index))) {
            continue;
        }
        QJsonObject comment = physicalComments.at(index);
        const QList<IndexedFragment> continuations = validFragmentsByRoot.value(static_cast<int>(index));
        if (!continuations.isEmpty()) {
            QJsonArray fragments {docsSnapshotFragment(comment)};
            QStringList selectedFragments;
            if (!comment.value(QStringLiteral("selected_text")).toString().isEmpty()) {
                selectedFragments.append(comment.value(QStringLiteral("selected_text")).toString());
            }
            for (const IndexedFragment &continuation : std::as_const(continuations)) {
                const QJsonObject physical = physicalComments.at(continuation.physicalIndex);
                fragments.append(docsSnapshotFragment(physical));
                if (!physical.value(QStringLiteral("selected_text")).toString().isEmpty()) {
                    selectedFragments.append(physical.value(QStringLiteral("selected_text")).toString());
                }
            }
            comment.insert(QStringLiteral("fragments"), fragments);
            comment.insert(QStringLiteral("selected_text"), selectedFragments.join(QLatin1Char('\n')));
        }
        comments.append(comment);
    }

    *snapshot = QJsonObject {
        {QStringLiteral("version"), 2},
        {QStringLiteral("coordinate_space"), QStringLiteral("normalized-default-page")},
        {QStringLiteral("source_pdf"), QFileInfo(documentUrl.toLocalFile()).absoluteFilePath()},
        {QStringLiteral("page_count"), static_cast<int>(m_document->pages())},
        {QStringLiteral("comments"), comments},
    };
    return true;
}

bool Part::swapSavedDocumentSilently(const QUrl &saveUrl)
{
    const Okular::DocumentViewport savedViewport = m_document->viewport();
    const int horizontalScroll = m_pageView->horizontalScrollBar()->value();
    const int verticalScroll = m_pageView->verticalScrollBar()->value();
    QWidget *const sidebarItem = m_sidebar->currentItem();
    const bool sidebarVisible = m_sidebar->isSidebarVisible();
    const QPointer<QWidget> focusedWidget = QApplication::focusWidget();
    QWidget *const partWidget = widget();
    const bool updatesEnabled = partWidget && partWidget->updatesEnabled();
    if (updatesEnabled) {
        partWidget->setUpdatesEnabled(false);
    }

    const bool swapped = m_document->swapBackingFile(saveUrl.toLocalFile(), saveUrl);
    if (swapped) {
        recordCurrentFileRevision();
        if (m_document->viewport().toString() != savedViewport.toString()) {
            m_document->setViewport(savedViewport);
        }
        m_pageView->horizontalScrollBar()->setValue(horizontalScroll);
        m_pageView->verticalScrollBar()->setValue(verticalScroll);
        if (m_sidebar->currentItem() != sidebarItem) {
            m_sidebar->setCurrentItem(sidebarItem);
        }
        if (m_sidebar->isSidebarVisible() != sidebarVisible) {
            m_sidebar->setSidebarVisibility(sidebarVisible);
        }
        if (focusedWidget && focusedWidget->isVisible()) {
            focusedWidget->setFocus(Qt::OtherFocusReason);
        }
    }

    if (updatesEnabled) {
        partWidget->setUpdatesEnabled(true);
        partWidget->update();
    }
    return swapped;
}

bool Part::syncDocsComments(const QUrl &documentUrl, QString *failureReason, const QJsonObject *commentsSnapshot, const QString &saveTiming, qint64 elapsedBeforeSyncMs)
{
    if (!shouldSyncDocsComments(documentUrl)) {
        return true;
    }

    const auto fail = [this, failureReason](const QString &reason) {
        if (failureReason) {
            *failureReason = reason;
        }
        if (m_docsCommentRefreshQueued) {
            QTimer::singleShot(0, this, [this] {
                if (m_docsCommentRefreshQueued) {
                    scheduleDocsCommentRefresh();
                }
            });
        }
        return false;
    };
    if (m_syncingDocsComments) {
        return fail(i18n("comment synchronization is already running"));
    }
    QScopedValueRollback<bool> syncingDocsComments(m_syncingDocsComments, true);

    const QString logDirectory = QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server/logs"));
    m_docsCommentSyncLogPath = QDir(logDirectory).filePath(QStringLiteral("thoughts-comment-sync.log"));
    if (!QDir().mkpath(logDirectory)) {
        return fail(i18n("the synchronization log directory could not be created at %1", QDir::toNativeSeparators(logDirectory)));
    }

    QJsonObject generatedSnapshot;
    if (!commentsSnapshot) {
        QString snapshotFailureReason;
        if (!writeDocsCommentSnapshot(documentUrl, &generatedSnapshot, &snapshotFailureReason)) {
            return fail(snapshotFailureReason);
        }
        commentsSnapshot = &generatedSnapshot;
    }

    DocsCommentSynchronizer synchronizer({
        .docsRoot = m_docsRenderRoot,
        .synctexExecutable = m_docsSyncTexExecutable,
        .processEnvironment = docsToolEnvironment(),
        .synctexTimeoutMs = m_docsSyncTexTimeoutMs,
        .lockTimeoutMs = 2000,
    });
    const DocsCommentSynchronizer::Result result = synchronizer.synchronize(documentUrl, *commentsSnapshot);

    qint64 sourceWatcherMs = 0;
    qint64 postSyncMs = 0;
    if (result.success) {
        m_fedoraRepository->captureDocument(QFileInfo(documentUrl.toLocalFile()).completeBaseName());
        QElapsedTimer postSyncTimer;
        postSyncTimer.start();
        QElapsedTimer phaseTimer;
        phaseTimer.start();
        applyDocsSourceWatchPaths(documentUrl, result.watchedSourceFiles, result.watchedSourceFilesComplete, false, true);
        m_docsSourceReconciledCommentFingerprint = result.visibleSidecarFingerprint;
        sourceWatcherMs = phaseTimer.elapsed();
        scheduleDocsCommentRefresh();
        if (m_docsSourceReconcileQueued) {
            scheduleDocsSourceReconciliation();
        }
        postSyncMs = postSyncTimer.elapsed();
    }

    QSaveFile logFile(m_docsCommentSyncLogPath);
    if (logFile.open(QIODevice::WriteOnly)) {
        QTextStream stream(&logFile);
        for (const QString &line : result.logLines) {
            stream << line << '\n';
        }
        stream << "timing: ";
        if (!saveTiming.isEmpty()) {
            stream << saveTiming << ' ';
        }
        stream << "native-sync-ms=" << result.totalMs << " source-watcher-ms=" << sourceWatcherMs << " post-sync-ms=" << postSyncMs
               << " total-ms=" << (elapsedBeforeSyncMs >= 0 ? elapsedBeforeSyncMs + result.totalMs + postSyncMs : result.totalMs + postSyncMs) << '\n';
        logFile.commit();
    }
    if (!result.success) {
        return fail(result.failureReason);
    }
    return true;
}

QJsonObject Part::currentDocsCommentRecord(const QString &annotationId) const
{
    QString error;
    const QList<QJsonObject> records = DocsCommentSynchronizer::visibleSidecarComments(
        m_docsRenderRoot, QFileInfo(localFilePath()).completeBaseName(), nullptr, &error);
    if (error.isEmpty()) {
        for (const QJsonObject &record : records) {
            if (record.value(QStringLiteral("id")).toString() == annotationId) {
                return record;
            }
        }
    }
    return m_docsCommentRecords.value(annotationId);
}

QJsonObject Part::currentCommentDetailsRecord(const QString &annotationId) const
{
    const QString stableId = Thoughts::logicalCommentId(annotationId);
    QJsonObject record = m_docsCommentRecords.value(stableId);
    const auto reading = Thoughts::ProjectReadingContext::load(localFilePath());
    const auto *child = reading ? reading->locate(record.value(QStringLiteral("page")).toInt() - 1) : nullptr;
    const QString stem = child ? child->stem() : QFileInfo(localFilePath()).completeBaseName();
    QString error;
    const auto records = DocsCommentSynchronizer::visibleSidecarComments(m_docsRenderRoot, stem, nullptr, &error);
    if (error.isEmpty()) {
        for (const QJsonObject &candidate : records) {
            if (candidate.value(QStringLiteral("id")).toString() == stableId) {
                record = child ? Thoughts::offsetCommentPages(candidate, child->document.firstPage) : candidate;
                break;
            }
        }
    }
    const QJsonObject projections = effectiveFedoraProjections();
    const QJsonObject annotation = projections.value(QStringLiteral("annotation\n") + stem + QLatin1Char('\n') + stableId).toObject();
    if (!annotation.isEmpty() && (record.isEmpty() || annotation.value(QStringLiteral("revision")).toInteger() > record.value(QStringLiteral("revision")).toInteger())) {
        record = child ? Thoughts::offsetCommentPages(annotation, child->document.firstPage) : annotation;
    }
    const QJsonObject logical = projections.value(QStringLiteral("logical_comment\ncomments\n") + stableId).toObject();
    if (!logical.isEmpty()) {
        // Thread purpose has no native PDF representation; the canonical record
        // remains authoritative even while a newer PDF text edit is pending.
        if (logical.contains(QStringLiteral("linking"))) record.insert(QStringLiteral("linking"), logical.value(QStringLiteral("linking")));
        record.insert(QStringLiteral("logical_comment_id"), stableId);
        record.insert(QStringLiteral("thread_root_id"), logical.value(QStringLiteral("thread_root_id")));
        record.insert(QStringLiteral("needs_action"), logical.value(QStringLiteral("needs_action")));
    }
    const QDateTime localUpdated = DocsCommentSynchronizer::parseCommentDate(record.value(QStringLiteral("updated_at")));
    const QDateTime logicalUpdated = DocsCommentSynchronizer::parseCommentDate(logical.value(QStringLiteral("updated_at")));
    if (!logical.isEmpty() && (!localUpdated.isValid() || !logicalUpdated.isValid() || logicalUpdated >= localUpdated)) {
        // Logical metadata includes the optimistic outbox overlay. Keep PDF
        // placement geometry from the annotation record for existing callers.
        for (const QString &field : {QStringLiteral("contents"), QStringLiteral("author"), QStringLiteral("priority"), QStringLiteral("location"), QStringLiteral("chatgpt_url"), QStringLiteral("deleted")}) {
            record.insert(field, logical.value(field));
        }
        record.insert(QStringLiteral("in_reply_to"), logical.value(QStringLiteral("parent_id")));
        record.insert(QStringLiteral("created"), logical.value(QStringLiteral("created_at")));
        record.insert(QStringLiteral("modified"), logical.value(QStringLiteral("updated_at")));
        record.insert(QStringLiteral("subtype"), logical.value(QStringLiteral("subtype")));
        const QJsonObject scope = logical.value(QStringLiteral("scope")).toObject();
        record.insert(QStringLiteral("tex_file"), logical.value(QStringLiteral("source_file")).toString(scope.value(QStringLiteral("source_file")).toString()));
        record.insert(QStringLiteral("tex_line"), logical.value(QStringLiteral("source_line")).toInt(scope.value(QStringLiteral("source_line")).toInt()));
        record.insert(QStringLiteral("id"), stableId);
    }
    return record;
}

bool Part::queueDocsCommentMutations(const QList<QJsonObject> &mutations, bool markHistoryClean, QString *failureReason)
{
    const auto fail = [failureReason](const QString &reason) {
        if (failureReason) {
            *failureReason = reason;
        }
        return false;
    };
    if (!shouldSyncDocsComments(url())) {
        return fail(i18n("This document does not use the Thoughts comment sidecar."));
    }
    if (!m_docsOpenedPdfIdentityVerified || m_docsOpenedPdfSha256.isEmpty()) {
        return fail(i18n("The immutable PDF identity has not been verified yet."));
    }
    if (!m_docsCommentMutationStore) {
        return fail(i18n("The pending comment journal is unavailable."));
    }
    const QString pdfStem = QFileInfo(url().toLocalFile()).completeBaseName();
    QString error;
    auto journaledMutations = mutations;
    for (auto &mutation : journaledMutations) {
        if (mutation.value(QStringLiteral("kind")).toString() == QLatin1String("upsert")
            && m_docsNewCommentIds.contains(mutation.value(QStringLiteral("id")).toString())) {
            mutation.insert(QStringLiteral("create_logical_comment"), true);
        }
    }
    const auto reading = Thoughts::ProjectReadingContext::load(url().toLocalFile());
    QMap<QString, QList<QJsonObject>> batches;
    for (auto mutation : journaledMutations) {
        QString stem = pdfStem;
        if (reading) {
            const auto record = mutation.value(QStringLiteral("record")).toObject();
            const auto previous = mutation.value(QStringLiteral("previous_record")).toObject();
            const auto geometry = record.isEmpty() ? previous : record;
            const auto *child = reading->locate(geometry.value(QStringLiteral("page")).toInt() - 1);
            if (!child) return fail(i18n("The comment has no verified document location."));
            const auto validPages = [&child](const QJsonObject &value) {
                const int page = value.value(QStringLiteral("page")).toInt() - 1;
                return page >= child->document.firstPage && page < child->document.firstPage + child->document.pages;
            };
            for (const auto &fragment : geometry.value(QStringLiteral("fragments")).toArray()) {
                if (!validPages(fragment.toObject())) return fail(i18n("A comment must belong to one document. Select text within a single document."));
            }
            stem = child->stem();
            mutation.insert(QStringLiteral("annotation_pdf_sha256"), child->document.pdfSha256);
            for (const auto &field : {QStringLiteral("record"), QStringLiteral("previous_record")}) {
                if (!mutation.value(field).isObject()) continue;
                auto local = Thoughts::offsetCommentPages(mutation.value(field).toObject(), -child->document.firstPage);
                local.insert(QStringLiteral("document_id"), child->document.id);
                local.insert(QStringLiteral("pdf_sha256"), child->document.pdfSha256);
                mutation.insert(field, local);
            }
        }
        batches[stem].append(mutation);
    }
    if (!m_docsCommentMutationStore->queueBatches(m_docsRenderRoot, batches, &error))
        return fail(i18n("The comment could not be written to the recovery journal: %1", error));
    if (markHistoryClean) {
        m_document->setHistoryClean(true);
    }
    const QVariantMap pendingChanges = globalPendingChanges();
    Q_EMIT pendingChangesChanged(QJsonObject::fromVariantMap(pendingChanges));
    scheduleDocsCommentMutationFlush();
    return true;
}

bool Part::queueDocsCommentUpsert(const QString &annotationId, const QJsonObject &metadataPatch, bool markHistoryClean, QString *failureReason)
{
    QJsonObject snapshot;
    QString error;
    if (!writeDocsCommentSnapshot(url(), &snapshot, &error, annotationId)) {
        if (failureReason) {
            *failureReason = error;
        }
        return false;
    }
    const QJsonArray comments = snapshot.value(QStringLiteral("comments")).toArray();
    if (comments.size() != 1 || comments.first().toObject().value(QStringLiteral("id")).toString() != annotationId) {
        if (failureReason) {
            *failureReason = i18n("The in-memory comment could not be serialized.");
        }
        return false;
    }
    const QString timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const QJsonObject mutation {
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("operation_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("kind"), QStringLiteral("upsert")},
        {QStringLiteral("id"), annotationId},
        {QStringLiteral("occurred_at"), timestamp},
        {QStringLiteral("annotation_pdf_sha256"), m_docsOpenedPdfSha256},
        {QStringLiteral("record"), comments.first()},
        {QStringLiteral("metadata_patch"), metadataPatch},
    };
    return queueDocsCommentMutations({mutation}, markHistoryClean, failureReason);
}

bool Part::queueDocsCommentDeletions(const QStringList &annotationIds, bool markHistoryClean, QString *failureReason)
{
    QList<QJsonObject> mutations;
    const QString timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    for (const QString &annotationId : annotationIds) {
        if (annotationId.isEmpty()) {
            continue;
        }
        mutations.append(QJsonObject {
            {QStringLiteral("schema_version"), 1},
            {QStringLiteral("operation_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("kind"), QStringLiteral("delete")},
            {QStringLiteral("id"), annotationId},
            {QStringLiteral("occurred_at"), timestamp},
            {QStringLiteral("annotation_pdf_sha256"), m_docsOpenedPdfSha256},
            {QStringLiteral("previous_record"), currentDocsCommentRecord(annotationId)},
        });
    }
    if (mutations.isEmpty()) {
        if (failureReason) {
            *failureReason = i18n("No comments were selected for deletion.");
        }
        return false;
    }
    return queueDocsCommentMutations(mutations, markHistoryClean, failureReason);
}

bool Part::queueDocsCommentRestores(const QList<QJsonObject> &records, bool markHistoryClean, QString *failureReason)
{
    QList<QJsonObject> mutations;
    const QString timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    for (const QJsonObject &record : records) {
        const QString id = record.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) {
            continue;
        }
        mutations.append(QJsonObject {
            {QStringLiteral("schema_version"), 1},
            {QStringLiteral("operation_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("kind"), QStringLiteral("restore")},
            {QStringLiteral("id"), id},
            {QStringLiteral("occurred_at"), timestamp},
            {QStringLiteral("annotation_pdf_sha256"), m_docsOpenedPdfSha256},
            {QStringLiteral("canonical_record"), record},
        });
    }
    if (mutations.isEmpty()) {
        if (failureReason) {
            *failureReason = i18n("No comments were available to restore.");
        }
        return false;
    }
    return queueDocsCommentMutations(mutations, markHistoryClean, failureReason);
}

void Part::handleDocsAnnotationMutation(int page, Okular::Annotation *annotation, Okular::Document::AnnotationMutation mutation)
{
    if (!annotation || m_applyingDocsSidecar || m_suppressDocsCommentMutationCapture || !shouldSyncDocsComments(url())) {
        return;
    }
    QString physicalAnnotationId = annotation->uniqueName();
    QString annotationId = physicalAnnotationId;
    if (const std::optional<Thoughts::CommentFragmentId> fragment = Thoughts::parseCommentFragmentId(annotationId)) {
        annotationId = fragment->logicalId;
    }
    const bool knownComment = !annotationId.isEmpty() && (m_docsCommentRecords.contains(annotationId) || m_docsAppliedCommentFingerprints.contains(annotationId));
    if (!knownComment && mutation == Okular::Document::AnnotationMutation::Added && !annotationId.isEmpty()) {
        m_docsNewCommentIds.insert(annotationId);
    }
    bool commentShape = false;
    if (annotation->subType() == Okular::Annotation::AText) {
        const auto *text = static_cast<const Okular::TextAnnotation *>(annotation);
        commentShape = text->textType() == Okular::TextAnnotation::Linked;
    } else if (annotation->subType() == Okular::Annotation::AHighlight) {
        commentShape = true;
    }
    const bool supportedComment = commentShape && !annotation->contents().trimmed().isEmpty();
    if (!knownComment && commentShape && !supportedComment) {
        if (mutation == Okular::Document::AnnotationMutation::Added && !annotationId.isEmpty()) {
            const bool cleanBeforeAddition = m_transientEmptyDocsCommentIds.value(annotationId, false) || m_document->isHistoryClean();
            m_transientEmptyDocsCommentIds.insert(annotationId, cleanBeforeAddition);
            QTimer::singleShot(0, this, [this, annotationId] {
                if (m_transientEmptyDocsCommentIds.value(annotationId, false) && m_document->isOpened()) {
                    m_document->setHistoryClean(true);
                }
            });
        }
        return;
    }
    if (!knownComment && !supportedComment) {
        return;
    }
    m_transientEmptyDocsCommentIds.remove(annotationId);
    if (annotation->flags() & (Okular::Annotation::BeingMoved | Okular::Annotation::BeingResized)) {
        if (m_pendingDocsCommentMutationIds.isEmpty()) {
            m_pendingDocsCommentPreHistoryIndex = m_document->undoHistoryIndex();
        }
        m_pendingDocsCommentWasClean = true;
        m_pendingDocsCommentMutationIds.insert(annotationId);
        return;
    }
    if (annotationId.isEmpty()) {
        if (mutation == Okular::Document::AnnotationMutation::Removed) {
            return;
        }
        annotationId = QStringLiteral("thoughts-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        annotation->setUniqueName(annotationId);
        annotation->setCreatorApplication(QStringLiteral("thoughts"));
        physicalAnnotationId = annotationId;
        m_docsNewCommentIds.insert(annotationId);
    }

    if (mutation == Okular::Document::AnnotationMutation::Removed) {
        m_docsProjectedAnnotations.remove(physicalAnnotationId);
    } else if (!physicalAnnotationId.isEmpty()) {
        m_docsProjectedAnnotations.insert(physicalAnnotationId, qMakePair(page, annotation));
    }

    if (m_pendingDocsCommentMutationIds.isEmpty()) {
        m_pendingDocsCommentPreHistoryIndex = m_document->undoHistoryIndex();
    }
    m_pendingDocsCommentWasClean = true;
    m_pendingDocsCommentMutationIds.insert(annotationId);
    m_docsCommentMutationCaptureTimer->start(0);
}

void Part::discardTransientEmptyDocsComment(Okular::Annotation *annotation, int page)
{
    if (!annotation || !shouldSyncDocsComments(url()) || !annotation->contents().trimmed().isEmpty()) {
        return;
    }

    const QString annotationId = Thoughts::logicalCommentId(annotation->uniqueName());
    if (annotationId.isEmpty() || !m_transientEmptyDocsCommentIds.contains(annotationId)) {
        return;
    }
    const bool restoreCleanState = m_transientEmptyDocsCommentIds.value(annotationId, false);
    const Thoughts::CommentFragmentGroup group = Thoughts::commentFragmentGroup(m_document, annotation, page);
    QList<Okular::Document::PageAnnotation> annotations;
    for (const Thoughts::CommentFragment &fragment : group.fragments) {
        const Okular::Page *fragmentPage = fragment.page >= 0 && fragment.page < static_cast<int>(m_document->pages()) ? m_document->page(fragment.page) : nullptr;
        if (fragment.annotation && fragmentPage && fragmentPage->annotations().contains(fragment.annotation)) {
            annotations.append(qMakePair(fragment.page, fragment.annotation));
        }
    }
    if (!annotations.isEmpty()) {
        QScopedValueRollback<bool> suppressCapture(m_suppressDocsCommentMutationCapture, true);
        m_document->removePageAnnotations(annotations);
    }
    if (restoreCleanState) {
        QTimer::singleShot(0, this, [this, annotationId] {
            m_transientEmptyDocsCommentIds.remove(annotationId);
            if (m_document->isOpened()) {
                m_document->setHistoryClean(true);
            }
        });
    } else {
        m_transientEmptyDocsCommentIds.remove(annotationId);
    }
}

void Part::capturePendingDocsCommentMutations()
{
    if (m_pendingDocsCommentMutationIds.isEmpty() || !m_document->isOpened()) {
        return;
    }
    QStringList ids(m_pendingDocsCommentMutationIds.cbegin(), m_pendingDocsCommentMutationIds.cend());
    std::sort(ids.begin(), ids.end());
    m_pendingDocsCommentMutationIds.clear();
    const int previousHistoryIndex = m_pendingDocsCommentPreHistoryIndex;
    const bool markHistoryClean = m_pendingDocsCommentWasClean;
    m_pendingDocsCommentPreHistoryIndex = -1;
    m_pendingDocsCommentWasClean = false;

    QList<QJsonObject> mutations;
    const QString timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    for (const QString &id : std::as_const(ids)) {
        QJsonObject snapshot;
        QString serializationError;
        if (!writeDocsCommentSnapshot(url(), &snapshot, &serializationError, id)) {
            errorMessage(i18n("The comment could not be autosaved: %1", serializationError), 15000);
            return;
        }
        const QJsonArray comments = snapshot.value(QStringLiteral("comments")).toArray();
        if (comments.size() == 1) {
            mutations.append(QJsonObject {
                {QStringLiteral("schema_version"), 1},
                {QStringLiteral("operation_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                {QStringLiteral("kind"), QStringLiteral("upsert")},
                {QStringLiteral("id"), id},
                {QStringLiteral("occurred_at"), timestamp},
                {QStringLiteral("annotation_pdf_sha256"), m_docsOpenedPdfSha256},
                {QStringLiteral("record"), comments.first()},
            });
        } else {
            mutations.append(QJsonObject {
                {QStringLiteral("schema_version"), 1},
                {QStringLiteral("operation_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                {QStringLiteral("kind"), QStringLiteral("delete")},
                {QStringLiteral("id"), id},
                {QStringLiteral("occurred_at"), timestamp},
                {QStringLiteral("annotation_pdf_sha256"), m_docsOpenedPdfSha256},
                {QStringLiteral("previous_record"), currentDocsCommentRecord(id)},
            });
        }
    }

    QString error;
    if (queueDocsCommentMutations(mutations, markHistoryClean, &error)) {
        return;
    }
    const int currentHistoryIndex = m_document->undoHistoryIndex();
    QScopedValueRollback<bool> suppressCapture(m_suppressDocsCommentMutationCapture, true);
    if (currentHistoryIndex > previousHistoryIndex && m_document->canUndo()) {
        m_document->undo();
    } else if (currentHistoryIndex < previousHistoryIndex && m_document->canRedo()) {
        m_document->redo();
    }
    errorMessage(i18n("The comment change was reverted because it could not be journaled: %1", error), 15000);
}

void Part::scheduleDocsCommentMutationFlush(int delayMs)
{
    if (!m_docsCommentMutationFlushTimer) {
        return;
    }
    m_docsCommentMutationFlushTimer->start(qMax(0, delayMs));
}

void Part::startDocsCommentMutationFlush()
{
    if (!m_docsCommentMutationStore || !m_document->isOpened() || m_docsCommentMutationFlushWatcher->isRunning()) {
        return;
    }
    QString pdfStem = QFileInfo(url().toLocalFile()).completeBaseName();
    if (const auto reading = Thoughts::ProjectReadingContext::load(url().toLocalFile())) {
        for (const auto &child : reading->documents) {
            if (m_docsCommentMutationStore->hasPending(m_docsRenderRoot, child.stem())) { pdfStem = child.stem(); break; }
        }
    }
    if (!m_docsCommentMutationStore->hasPending(m_docsRenderRoot, pdfStem)) {
        return;
    }
    const DocsCommentMutationStore store = *m_docsCommentMutationStore;
    const QString docsRoot = m_docsRenderRoot;
    QPromise<DocsCommentMutationFlushResult> promise;
    const QFuture<DocsCommentMutationFlushResult> future = promise.future();
    promise.start();
    m_docsCommentMutationFlushWatcher->setFuture(future);
    QThreadPool::globalInstance()->start([promise = std::move(promise), store, docsRoot, pdfStem]() mutable {
        promise.addResult(store.flush(docsRoot, pdfStem));
        promise.finish();
    });
}

void Part::finishDocsCommentMutationFlush()
{
    const DocsCommentMutationFlushResult result = m_docsCommentMutationFlushWatcher->result();
    const auto retry = [this](const QString &reason) {
        const int delay = qMin(60000, 1000 * (1 << qMin(m_docsCommentMutationRetryAttempt++, 6)));
        errorMessage(i18n("The comment remains safe in the recovery journal, but synchronization is pending: %1", reason), 15000);
        scheduleDocsCommentMutationFlush(delay);
    };
    if (!result.success) {
        retry(result.failureReason);
        return;
    }
    if (result.appliedLines.isEmpty()) {
        m_docsCommentMutationRetryAttempt = 0;
        return;
    }
    const auto reading = Thoughts::ProjectReadingContext::load(m_docsCommentPdfPath);
    const auto *child = reading ? reading->forStem(result.pdfStem) : nullptr;
    const auto capturedSha = child ? child->document.pdfSha256 : m_docsOpenedPdfSha256;
    if (!m_fedoraRepository->captureCommentRecords(result.docsRoot, result.pdfStem, result.changedRecords, capturedSha, result.createdCommentIds)) {
        retry(i18n("the outbox for the server could not be updated"));
        return;
    }
    QString error;
    if (!m_docsCommentMutationStore->acknowledge(result.docsRoot, result.pdfStem, result.appliedLines, &error)) {
        retry(error);
        return;
    }
    Q_EMIT pendingChangesChanged(QJsonObject::fromVariantMap(globalPendingChanges()));

    m_docsCommentMutationRetryAttempt = 0;
    const bool sameDocument = QFileInfo(result.docsRoot).absoluteFilePath() == QFileInfo(m_docsRenderRoot).absoluteFilePath()
        && (child || result.pdfStem == QFileInfo(m_docsCommentPdfPath).completeBaseName());
    if (sameDocument) {
        for (const QJsonObject &localRecord : result.changedRecords) {
            const auto record = child ? Thoughts::offsetCommentPages(localRecord, child->document.firstPage) : localRecord;
            const QString id = record.value(QStringLiteral("id")).toString();
            if (record.value(QStringLiteral("deleted")).toBool()) {
                m_docsCommentRecords.remove(id);
                m_docsAppliedCommentFingerprints.remove(id);
            } else {
                m_docsCommentRecords.insert(id, record);
                m_docsAppliedCommentFingerprints.insert(id, docsCommentRenderFingerprint(record));
            }
        }
        if (!result.visibleFingerprint.isEmpty()) {
            m_docsCommentFingerprint = result.visibleFingerprint;
        }
        scheduleDocsCommentRefresh();
    }
    if (sameDocument && (reading || m_docsCommentMutationStore->hasPending(result.docsRoot, result.pdfStem))) {
        scheduleDocsCommentMutationFlush();
    }
}

void Part::startDocsPdfIdentityVerification(const QUrl &documentUrl, const QString &openedPathOverride)
{
    m_docsOpenedPdfIdentityVerified = false;
    m_docsOpenedPdfSha256.clear();
    m_docsOpenedPdfPath.clear();
    if (!documentUrl.isLocalFile() || !shouldSyncDocsComments(documentUrl)) {
        return;
    }
    const QString logicalPath = QFileInfo(documentUrl.toLocalFile()).absoluteFilePath();
    const QString openedPath = openedPathOverride.isEmpty() ? QFileInfo(documentUrl.toLocalFile()).canonicalFilePath() : QFileInfo(openedPathOverride).absoluteFilePath();
    if (openedPath.isEmpty()) {
        return;
    }

    // Library activation already verified every file before publishing the
    // immutable generation. Trust that exact manifest identity immediately so
    // UI mutations need not wait for another multi-megabyte hash; the worker
    // below re-hashes the same resolved path as an integrity check.
    const QFileInfo openedInfo(openedPath);
    const QString stagePrefix = QDir(m_docsRenderRoot).filePath(QStringLiteral(".thoughts-library-stage-"));
    if (openedInfo.absolutePath().startsWith(stagePrefix)) {
        QFile indexFile(QDir(openedInfo.absolutePath()).filePath(QStringLiteral(".thoughts-library-index.json")));
        const QJsonObject index = indexFile.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(indexFile.readAll()).object() : QJsonObject();
        QJsonObject selected;
        for (const QJsonValue &value : index.value(QStringLiteral("pdfs")).toArray()) {
            const QJsonObject entry = value.toObject();
            if (entry.value(QStringLiteral("filename")).toString() == openedInfo.fileName()) {
                selected = entry;
                break;
            }
        }
        const QString expected = selected.value(QStringLiteral("pdf_sha256")).toString().toLower();
        static const QRegularExpression sha256Pattern(QStringLiteral("^[0-9a-f]{64}$"));
        bool verified = sha256Pattern.match(expected).hasMatch();
        if (verified && selected.value(QStringLiteral("kind")).toString() == QLatin1String("source_linked")) {
            QFile manifestFile(QDir(openedInfo.absolutePath()).filePath(QStringLiteral(".thoughts-generation.json")));
            const QJsonObject manifest = manifestFile.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(manifestFile.readAll()).object() : QJsonObject();
            bool publicationMatches = false;
            for (const QJsonValue &value : manifest.value(QStringLiteral("publications")).toArray()) {
                const QJsonObject publication = value.toObject();
                if (publication.value(QStringLiteral("filename")).toString() == openedInfo.fileName()
                    && publication.value(QStringLiteral("sha256")).toString().toLower() == expected) {
                    publicationMatches = true;
                    break;
                }
            }
            verified = publicationMatches && !index.value(QStringLiteral("library_revision")).toString().isEmpty()
                && manifest.value(QStringLiteral("library_revision")) == index.value(QStringLiteral("library_revision"))
                && manifest.value(QStringLiteral("source_index_sha256")) == index.value(QStringLiteral("source_index_sha256"));
        }
        if (verified) {
            m_docsOpenedPdfPath = openedPath;
            m_docsOpenedPdfSha256 = expected;
            m_docsOpenedPdfIdentityVerified = true;
        }
    }
    QPromise<QJsonObject> promise;
    const QFuture<QJsonObject> future = promise.future();
    promise.start();
    m_docsPdfIdentityWatcher->setFuture(future);
    QThreadPool::globalInstance()->start([promise = std::move(promise), logicalPath, openedPath]() mutable {
        QJsonObject result {{QStringLiteral("logical_path"), logicalPath}, {QStringLiteral("opened_path"), openedPath}};
        const QByteArray digestBytes = docsSourceFingerprint(openedPath);
        const QString digest = QString::fromLatin1(digestBytes.toHex());
        static const QRegularExpression sha256Pattern(QStringLiteral("^[0-9a-f]{64}$"));
        if (!sha256Pattern.match(digest).hasMatch()) {
            result.insert(QStringLiteral("error"), QStringLiteral("the opened PDF could not be hashed"));
            promise.addResult(result);
            promise.finish();
            return;
        }

        const QFileInfo openedInfo(openedPath);
        if (openedInfo.suffix() == QLatin1String("thoughts-project")) {
            const auto reading = Thoughts::ProjectReadingContext::load(openedPath);
            QFile generationFile(QDir(openedInfo.absolutePath()).filePath(QStringLiteral(".thoughts-generation.json")));
            const auto generation = generationFile.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(generationFile.readAll()).object() : QJsonObject();
            bool matched = false;
            for (const auto &value : generation.value(QStringLiteral("project_readings")).toArray()) {
                const auto entry = value.toObject();
                matched |= reading && entry.value(QStringLiteral("filename")).toString() == openedInfo.fileName()
                    && entry.value(QStringLiteral("sha256")).toString() == digest
                    && entry.value(QStringLiteral("project_id")).toString() == reading->reading.id;
            }
            if (matched) result.insert(QStringLiteral("sha256"), digest);
            else result.insert(QStringLiteral("error"), QStringLiteral("the project reading generation is not verified"));
            promise.addResult(result); promise.finish(); return;
        }
        docsVerifiedCommentLocations(QFileInfo(logicalPath).fileName(), digest);
        QFile indexFile(QDir(openedInfo.absolutePath()).filePath(QStringLiteral(".thoughts-library-index.json")));
        const QJsonObject index = indexFile.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(indexFile.readAll()).object() : QJsonObject();
        QJsonObject selected;
        for (const QJsonValue &value : index.value(QStringLiteral("pdfs")).toArray()) {
            const QJsonObject entry = value.toObject();
            if (entry.value(QStringLiteral("filename")).toString() == openedInfo.fileName()) {
                selected = entry;
                break;
            }
        }
        const QString expected = selected.value(QStringLiteral("pdf_sha256")).toString().toLower();
        const bool sourceLinked = selected.value(QStringLiteral("kind")).toString() == QLatin1String("source_linked");
        if (!expected.isEmpty() && (!sha256Pattern.match(expected).hasMatch() || expected != digest)) {
            result.insert(QStringLiteral("error"), QStringLiteral("the opened PDF does not match its Library identity"));
            promise.addResult(result);
            promise.finish();
            return;
        }
        if (sourceLinked) {
            QFile manifestFile(QDir(openedInfo.absolutePath()).filePath(QStringLiteral(".thoughts-generation.json")));
            const QJsonObject manifest = manifestFile.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(manifestFile.readAll()).object() : QJsonObject();
            bool publicationMatches = false;
            for (const QJsonValue &value : manifest.value(QStringLiteral("publications")).toArray()) {
                const QJsonObject publication = value.toObject();
                if (publication.value(QStringLiteral("filename")).toString() == openedInfo.fileName() && publication.value(QStringLiteral("sha256")).toString() == digest) {
                    publicationMatches = true;
                    break;
                }
            }
            const bool frontierMatches = !index.value(QStringLiteral("library_revision")).toString().isEmpty()
                && manifest.value(QStringLiteral("library_revision")) == index.value(QStringLiteral("library_revision"))
                && manifest.value(QStringLiteral("source_index_sha256")) == index.value(QStringLiteral("source_index_sha256"));
            if (!publicationMatches || !frontierMatches) {
                result.insert(QStringLiteral("error"), QStringLiteral("the source-linked PDF generation is not verified"));
                promise.addResult(result);
                promise.finish();
                return;
            }
        }
        result.insert(QStringLiteral("sha256"), digest);
        promise.addResult(result);
        promise.finish();
    });
}

void Part::finishDocsPdfIdentityVerification()
{
    const QJsonObject result = m_docsPdfIdentityWatcher->result();
    const QString logicalPath = QFileInfo(result.value(QStringLiteral("logical_path")).toString()).absoluteFilePath();
    if (!m_document->isOpened() || logicalPath != QFileInfo(m_docsCommentPdfPath).absoluteFilePath()) {
        return;
    }
    const QString error = result.value(QStringLiteral("error")).toString();
    if (!error.isEmpty()) {
        m_docsOpenedPdfPath.clear();
        m_docsOpenedPdfSha256.clear();
        m_docsOpenedPdfIdentityVerified = false;
        errorMessage(i18n("Comments are read-only until the Library identity is repaired: %1", error), 15000);
        return;
    }
    m_docsOpenedPdfPath = result.value(QStringLiteral("opened_path")).toString();
    m_docsOpenedPdfSha256 = result.value(QStringLiteral("sha256")).toString();
    m_docsOpenedPdfIdentityVerified = !m_docsOpenedPdfPath.isEmpty() && m_docsOpenedPdfSha256.size() == 64;
    refreshDocumentFeedback();
    scheduleDocsCommentRefresh();
    if (m_docsOpenedPdfIdentityVerified && m_docsCommentMutationStore
        && m_docsCommentMutationStore->hasPending(m_docsRenderRoot, QFileInfo(m_docsCommentPdfPath).completeBaseName())) {
        scheduleDocsCommentMutationFlush(0);
    }
}

QVariantMap Part::synchronizeDocsCommentsNow()
{
    QString failureReason;
    const bool success = syncDocsComments(url(), &failureReason);
    return {
        {QStringLiteral("success"), success},
        {QStringLiteral("error"), failureReason},
    };
}

void Part::updateFromFedora(const QString &traceId)
{
    if (m_fedoraRepository) {
        m_manualSyncRequestedByPart = true;
        m_fedoraRepository->syncNow(traceId, true);
    }
}

void Part::updateFromFedoraWithoutRenderCheck(const QString &traceId)
{
    if (m_fedoraRepository) {
        m_manualSyncRequestedByPart = true;
        m_fedoraRepository->syncNow(traceId, false);
    }
}

void Part::updateDocumentFromFedora(const QString &target, bool publishSources, const QString &traceId)
{
    if (!m_fedoraRepository || !target.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive)
        || QFileInfo(target).fileName() != target) {
        return;
    }
    m_manualSyncRequestedByPart = true;
    m_fedoraRepository->captureDocument(QFileInfo(target).completeBaseName());
    m_fedoraRepository->syncDocument(target, publishSources, traceId);
}

QJsonObject Part::effectiveFedoraProjections() const
{
    return m_fedoraRepository ? m_fedoraRepository->effectiveProjections() : QJsonObject();
}

void Part::setSidebarReviewCards(const QJsonArray &cards)
{
    m_reviewsWidget->setReviewCards(cards);
    // A retired card stays on the PDF version it was retired on: the HTML view and the
    // attachments show the current one, so they leave it out.
    QJsonArray current;
    for (const QJsonValue &card : cards) {
        if (card.toObject().value(QStringLiteral("sidebar_schedule")).toObject().value(QStringLiteral("state")).toString() != QLatin1String("retired")) current.append(card);
    }
    if (m_htmlReader) m_htmlReader->setCards(current);
    // Cards made in HTML attachments of this document: highlighted in their page.
    m_sidebarCardRecords = current;
    if (m_attachmentReader) m_attachmentReader->setCards(current);
}

bool Part::focusSidebarReviewCard(const QString &cardId)
{
    return m_reviewsWidget && m_reviewsWidget->selectReviewCard(cardId);
}

void Part::openNoteAttachment(const QVariantMap &attachment)
{
    FedoraRepository::openNoteAttachment(m_docsRenderRoot, QJsonObject::fromVariantMap(attachment), this,
        [this](const QString &error) { KMessageBox::error(widget(), error); });
}

bool Part::enqueueLogicalCommentMutation(const QVariantMap &payload)
{
    return m_fedoraRepository && m_fedoraRepository->enqueueLogicalComment(QJsonObject::fromVariantMap(payload));
}

bool Part::enqueueDocumentFeedbackRecord(const QVariantMap &record)
{
    QString error;
    const bool queued = m_fedoraRepository && m_fedoraRepository->enqueueDocumentFeedback(QJsonObject::fromVariantMap(record), &error);
    if (!queued && !error.isEmpty()) qWarning().noquote() << "Could not queue the comment:" << error;
    return queued;
}

bool Part::openDocumentFeedback(const QString &feedbackId)
{
    if (!m_pageView) return false;
    auto *feedback = m_pageView->documentFeedback();
    const QJsonObject record = feedback->record(feedbackId);
    if (record.isEmpty()) return false;
    // Asked for by name, so it shows even if its version has passed.
    if (!feedback->isCurrent(record)) feedback->setShowEarlier(true);
    if (showAnnotationsForNewEntry()) m_reviewsWidget->selectFeedback(feedbackId);
    showFeedback(feedbackId, false);
    return true;
}

bool Part::enqueueAgentConversation(const QVariantMap &record)
{
    QString error;
    const bool queued = m_fedoraRepository && m_fedoraRepository->enqueueAgentConversation(QJsonObject::fromVariantMap(record), &error);
    if (!queued && !error.isEmpty()) qWarning().noquote() << "Could not queue the agent conversation:" << error;
    return queued;
}

QVariantMap Part::restoreCommentAnchors(const QStringList &ids)
{
    if (!m_fedoraRepository) {
        return {{QStringLiteral("success"), false}, {QStringLiteral("results"), QVariantList()},
                {QStringLiteral("message"), i18n("Anchors can only be restored while the Thoughts server is configured.")}};
    }
    const QVariantList results = m_fedoraRepository->restoreRemovedAnchors(ids);
    QStringList messages;
    bool success = !results.isEmpty();
    for (const QVariant &value : results) {
        const QVariantMap result = value.toMap();
        success = success && result.value(QStringLiteral("success")).toBool();
        const QString message = result.value(QStringLiteral("message")).toString();
        if (!message.isEmpty()) messages.append(message);
    }
    if (success && !messages.isEmpty()) messages.append(i18n("The source change is published at the next Sync."));
    return {{QStringLiteral("success"), success}, {QStringLiteral("results"), results}, {QStringLiteral("message"), messages.join(QLatin1Char('\n'))}};
}

namespace
{
QVariantMap removedProposalOutcome(const QVariantList &results, const QString &done)
{
    QStringList messages;
    bool success = !results.isEmpty();
    for (const QVariant &value : results) {
        const QVariantMap result = value.toMap();
        success = success && result.value(QStringLiteral("success")).toBool();
        const QString message = result.value(QStringLiteral("message")).toString();
        if (!message.isEmpty()) messages.append(message);
    }
    if (success && !done.isEmpty()) messages.append(done);
    return {{QStringLiteral("success"), success}, {QStringLiteral("results"), results}, {QStringLiteral("message"), messages.join(QLatin1Char('\n'))}};
}
}

QVariantMap Part::restoreRemovedProposals(const QStringList &ids)
{
    if (!m_fedoraRepository) {
        return {{QStringLiteral("success"), false}, {QStringLiteral("results"), QVariantList()},
                {QStringLiteral("message"), i18n("Proposals can only be restored while the Thoughts server is configured.")}};
    }
    return removedProposalOutcome(m_fedoraRepository->restoreRemovedProposals(ids), QString());
}

QVariantMap Part::dismissRemovedProposals(const QStringList &ids)
{
    if (!m_fedoraRepository) {
        return {{QStringLiteral("success"), false}, {QStringLiteral("results"), QVariantList()},
                {QStringLiteral("message"), i18n("Proposals can only be dismissed while the Thoughts server is configured.")}};
    }
    return removedProposalOutcome(m_fedoraRepository->dismissRemovedProposals(ids),
                                  i18np("Proposal dismissed; the next Sync dismisses it on your other devices.",
                                        "%1 proposals dismissed; the next Sync dismisses them on your other devices.", ids.size()));
}

QVariantMap Part::resolveMissingCommentMarkers(const QString &action, const QVariantList &markers)
{
    if (!m_fedoraRepository || (action != QLatin1String("remove") && action != QLatin1String("restore"))) {
        return {{QStringLiteral("success"), false}, {QStringLiteral("results"), QVariantList()},
                {QStringLiteral("message"), i18n("Missing comments can only be resolved while the Thoughts server is configured.")}};
    }
    const QVariantList results = action == QLatin1String("remove") ? m_fedoraRepository->removeMissingCommentMarkers(markers)
                                                                    : m_fedoraRepository->restoreMissingComments(markers, Okular::Settings::identityAuthor());
    QStringList messages;
    bool success = !results.isEmpty();
    for (const QVariant &value : results) {
        const QVariantMap result = value.toMap();
        success = success && result.value(QStringLiteral("success")).toBool();
        const QString message = result.value(QStringLiteral("message")).toString();
        if (!message.isEmpty()) messages.append(message);
    }
    if (success && !messages.isEmpty()) messages.append(i18n("The change is published at the next Sync."));
    return {{QStringLiteral("success"), success}, {QStringLiteral("results"), results}, {QStringLiteral("message"), messages.join(QLatin1Char('\n'))}};
}

bool Part::enqueueReviewCorrespondenceReview(const QVariantMap &payload)
{
    return m_fedoraRepository && m_fedoraRepository->enqueueReviewCorrespondenceReview(QJsonObject::fromVariantMap(payload));
}

QVariantMap Part::documentSyncStatus(const QString &target) const
{
    QJsonObject status = m_fedoraRepository ? m_fedoraRepository->pendingDocumentChanges(target) : QJsonObject();
    const QString targetStem = QFileInfo(target).completeBaseName();
    const QString activeStem = QFileInfo(m_docsCommentPdfPath).completeBaseName();
    const bool pendingJournal = m_docsCommentMutationStore && targetStem == activeStem
        && m_docsCommentMutationStore->hasPending(m_docsRenderRoot, targetStem);
    if (pendingJournal) {
        const int comments = qMax(1, status.value(QStringLiteral("comments")).toInt());
        status.insert(QStringLiteral("comments"), comments);
        status.insert(QStringLiteral("total"), comments + status.value(QStringLiteral("review_cards")).toInt() + status.value(QStringLiteral("review_events")).toInt());
        status.insert(QStringLiteral("pending_comment_journal"), true);
        status.insert(QStringLiteral("available"), true);
    }
    return status.toVariantMap();
}

QVariantMap Part::globalPendingChanges() const
{
    QJsonObject status = m_fedoraRepository ? m_fedoraRepository->pendingChanges() : QJsonObject();
    const QString targetStem = QFileInfo(m_docsCommentPdfPath).completeBaseName();
    const bool pendingJournal = m_docsCommentMutationStore && !targetStem.isEmpty()
        && m_docsCommentMutationStore->hasPending(m_docsRenderRoot, targetStem);
    const QString target = targetStem.isEmpty() ? QString() : targetStem + QStringLiteral(".pdf");
    const QJsonObject document = target.isEmpty() || !m_fedoraRepository ? QJsonObject() : m_fedoraRepository->pendingDocumentChanges(target);
    if (pendingJournal && document.value(QStringLiteral("comments")).toInt() == 0) {
        status.insert(QStringLiteral("available"), true);
        status.insert(QStringLiteral("comments"), status.value(QStringLiteral("comments")).toInt() + 1);
        status.insert(QStringLiteral("total"), status.value(QStringLiteral("total")).toInt() + 1);
        status.insert(QStringLiteral("pending_comment_journal"), true);
    }
    return status.toVariantMap();
}

QString Part::resolveAgentProposals(const QString &bundleId, const QString &action, const QStringList &proposalIds)
{
    if (!m_fedoraRepository) {
        return {};
    }
    const QString operationId = m_fedoraRepository->resolveAgentProposals(bundleId, action, proposalIds);
    if (!operationId.isEmpty()) {
        m_pendingAgentProposalResolutions.insert(operationId, {proposalIds, action, false});
    }
    return operationId;
}

void Part::requestSourceConflictResolutionBundle(const QString &path)
{
    if (m_fedoraRepository) {
        m_fedoraRepository->requestSourceConflictResolutionBundle(path);
    }
}

QString Part::queueSourceConflictResolution(const QString &conflictId,
                                            qint64 expectedRevision,
                                            const QString &expectedSha256,
                                            const QByteArray &contents)
{
    return m_fedoraRepository ? m_fedoraRepository->queueSourceConflictResolution(conflictId, expectedRevision, expectedSha256, contents) : QString();
}

bool Part::selectAgentProposal(const QString &proposalId)
{
    return m_pageView && m_pageView->selectAgentProposal(proposalId);
}

QVariantMap Part::focusAgentProposal(const QString &proposalId, const QString &sourcePath, int sourceLine)
{
    if (!m_pageView || !m_document) {
        return {{QStringLiteral("focused"), false}, {QStringLiteral("pending"), false}, {QStringLiteral("error"), i18n("The PDF view is unavailable.")}};
    }
    if (m_pageView->selectAgentProposal(proposalId)) {
        return {{QStringLiteral("focused"), true}, {QStringLiteral("pending"), false}, {QStringLiteral("mode"), QStringLiteral("overlay")}};
    }
    if (!m_pageView->agentProposalOverlaysReady()) {
        return {{QStringLiteral("focused"), false}, {QStringLiteral("pending"), true}};
    }
    if (sourcePath.isEmpty() || sourceLine <= 0) {
        return {{QStringLiteral("focused"), false}, {QStringLiteral("pending"), false}, {QStringLiteral("error"), i18n("No TeX source location is available for this proposal.")}};
    }
    const QString sourceReference = QStringLiteral("src:%1 %2").arg(sourceLine).arg(sourcePath);
    const DocumentViewport viewport(m_document->metaData(QStringLiteral("NamedViewport"), sourceReference).toString());
    if (!viewport.isValid()) {
        return {{QStringLiteral("focused"), false}, {QStringLiteral("pending"), false}, {QStringLiteral("error"), i18n("SyncTeX could not locate this proposal in the current PDF.")}};
    }
    m_document->setViewport(viewport, nullptr, true);
    m_pageView->setLastSourceLocationViewport(viewport);
    return {{QStringLiteral("focused"), true}, {QStringLiteral("pending"), false}, {QStringLiteral("mode"), QStringLiteral("synctex")}};
}

QVariantMap Part::openAgentProposalReview(const QString &proposalId)
{
    return openAgentProposalReview(proposalId, {}, 0);
}

QVariantMap Part::openAgentProposalReview(const QString &proposalId, const QString &sourcePath, int sourceLine)
{
    Q_UNUSED(sourceLine); // Resolve the stable ID in the current source; overview line numbers can be stale.
    if (!m_pageView || !m_reviewsWidget || proposalId.isEmpty()) {
        return {{QStringLiteral("opened"), false}, {QStringLiteral("pending"), false}, {QStringLiteral("error"), i18n("The proposal review is unavailable.")}};
    }
    QString sourceError;
    if (!sourcePath.isEmpty() && m_reviewsWidget->agentProposal(proposalId).id != proposalId) {
        sourceError = m_reviewsWidget->loadAgentProposalSource(proposalId, sourcePath);
    }
    const AgentProposalSidebarEntry proposal = m_reviewsWidget->agentProposal(proposalId);
    if (proposal.id == proposalId) {
        const bool proposedTextIsEmpty = proposal.after.trimmed().isEmpty();
        m_currentAgentProposalReviewId = proposalId;
        m_locatedAgentProposalReviewId.clear();
        m_currentAgentProposalText = proposedTextIsEmpty ? proposal.before : proposal.after;
        m_currentAgentProposalFallbackText = proposedTextIsEmpty ? QString() : proposal.before;
        m_currentAgentProposalSourcePath = proposal.sourcePath;
        m_currentAgentProposalSourceLine = proposal.sourceLine;
        m_currentAgentProposalTargetsCurrentText = proposedTextIsEmpty;
        m_pageView->openAgentProposalWindow(proposal);
        locateOpenAgentProposal();
        return {{QStringLiteral("opened"), true}, {QStringLiteral("pending"), false}};
    }
    if (m_reviewsWidget->agentProposalRefreshPending()) {
        return {{QStringLiteral("opened"), false}, {QStringLiteral("pending"), true}};
    }
    return {{QStringLiteral("opened"), false},
            {QStringLiteral("pending"), false},
            {QStringLiteral("error"), sourceError.isEmpty()
                 ? i18n("This proposal could not be loaded for the current PDF. This does not mean it was accepted or rejected. Open it from the overview to check its TeX source.")
                 : sourceError}};
}

void Part::locateOpenAgentProposal()
{
    if (!m_pageView || !m_reviewsWidget || m_currentAgentProposalReviewId.isEmpty()
        || m_pageView->openAgentProposalId() != m_currentAgentProposalReviewId) {
        return;
    }
    if (m_pageView->selectAgentProposal(m_currentAgentProposalReviewId)) {
        m_locatedAgentProposalReviewId = m_currentAgentProposalReviewId;
        m_pageView->clearAgentProposalTextHighlight();
        m_pendingAgentProposalTextSearchId.clear();
        if (!m_agentProposalTextSearchRunning) {
            m_document->resetSearch(AgentProposalSearchId);
        }
        return;
    }
    if (m_locatedAgentProposalReviewId == m_currentAgentProposalReviewId) {
        return;
    }
    if (!m_pageView->agentProposalOverlaysReady()) {
        return;
    }
    startAgentProposalTextSearch(m_currentAgentProposalReviewId,
                                 m_currentAgentProposalText,
                                 m_currentAgentProposalFallbackText,
                                 m_currentAgentProposalTargetsCurrentText,
                                 m_currentAgentProposalSourcePath,
                                 m_currentAgentProposalSourceLine);
}

void Part::startAgentProposalTextSearch(const QString &proposalId,
                                        const QString &proposalText,
                                        const QString &fallbackText,
                                        bool targetsCurrentText,
                                        const QString &sourcePath,
                                        int sourceLine)
{
    const QString query = agentProposalSearchQuery(proposalText);
    if (query.isEmpty()) {
        if (!fallbackText.trimmed().isEmpty()) {
            startAgentProposalTextSearch(proposalId, fallbackText, QString(), true, sourcePath, sourceLine);
            return;
        }
        const QVariantMap result = focusAgentProposal(proposalId, sourcePath, sourceLine);
        if (!result.value(QStringLiteral("focused")).toBool() && !result.value(QStringLiteral("pending")).toBool()) {
            QTimer::singleShot(0, this, [this, proposalId] {
                if (m_pageView && m_currentAgentProposalReviewId == proposalId) {
                    m_pageView->showAgentProposalLocationUnavailable(proposalId);
                }
            });
        }
        return;
    }
    if (m_agentProposalTextSearchRunning) {
        if (m_agentProposalTextSearchId == proposalId
            && m_agentProposalTextSearchText == proposalText
            && m_agentProposalTextSearchFallbackText == fallbackText
            && m_agentProposalTextSearchTargetsCurrentText == targetsCurrentText) {
            return;
        }
        if (m_pendingAgentProposalTextSearchId == proposalId
            && m_pendingAgentProposalTextSearchText == proposalText
            && m_pendingAgentProposalTextSearchFallbackText == fallbackText
            && m_pendingAgentProposalTextSearchTargetsCurrentText == targetsCurrentText) {
            return;
        }
        m_pendingAgentProposalTextSearchId = proposalId;
        m_pendingAgentProposalTextSearchText = proposalText;
        m_pendingAgentProposalTextSearchFallbackText = fallbackText;
        m_pendingAgentProposalTextSearchSourcePath = sourcePath;
        m_pendingAgentProposalTextSearchSourceLine = sourceLine;
        m_pendingAgentProposalTextSearchTargetsCurrentText = targetsCurrentText;
        return;
    }

    m_pageView->clearAgentProposalTextHighlight();
    m_document->resetSearch(AgentProposalSearchId);
    m_agentProposalTextSearchId = proposalId;
    m_agentProposalTextSearchText = proposalText;
    m_agentProposalTextSearchFallbackText = fallbackText;
    m_agentProposalTextSearchSourcePath = sourcePath;
    m_agentProposalTextSearchSourceLine = sourceLine;
    m_agentProposalTextSearchTargetsCurrentText = targetsCurrentText;
    m_agentProposalTextSearchRunning = true;
    m_document->searchText(AgentProposalSearchId,
                           query,
                           true,
                           Qt::CaseInsensitive,
                           Document::NextMatch,
                           true,
                           QColor(255, 210, 0, 150));
}

void Part::finishAgentProposalTextSearch(Document::SearchStatus status)
{
    const QString completedProposalId = m_agentProposalTextSearchId;
    const QString completedProposalText = m_agentProposalTextSearchText;
    const QString completedFallbackText = m_agentProposalTextSearchFallbackText;
    const QString completedSourcePath = m_agentProposalTextSearchSourcePath;
    const int completedSourceLine = m_agentProposalTextSearchSourceLine;
    const bool completedSearchTargetsCurrentText = m_agentProposalTextSearchTargetsCurrentText;
    m_agentProposalTextSearchRunning = false;

    if (completedProposalId == m_currentAgentProposalReviewId && m_pageView->selectAgentProposal(completedProposalId)) {
        m_locatedAgentProposalReviewId = completedProposalId;
        m_document->resetSearch(AgentProposalSearchId);
        m_pendingAgentProposalTextSearchId.clear();
        return;
    }

    if (completedProposalId == m_currentAgentProposalReviewId && status == Document::MatchFound) {
        const QList<Document::SearchResult> matches = m_document->searchResults(AgentProposalSearchId);
        if (!matches.isEmpty() && !matches.constFirst().area.isEmpty()) {
            m_locatedAgentProposalReviewId = completedProposalId;
            const Document::SearchResult &match = matches.constFirst();
            const Okular::NormalizedRect &rect = match.area.constFirst();
            const Okular::Page *page = m_document->page(match.pageNumber);
            if (page) {
                std::unique_ptr<Okular::RegularAreaRect> endMatch;
                for (const QString &endQuery : agentProposalEndSearchQueries(completedProposalText)) {
                    endMatch.reset(page->findText(AgentProposalEndSearchId, endQuery, Okular::FromTop, Qt::CaseInsensitive));
                    if (endMatch && !endMatch->isEmpty() && endMatch->constLast().bottom >= rect.top) {
                        break;
                    }
                    endMatch.reset();
                }
                if (endMatch && !endMatch->isEmpty()) {
                    const Okular::NormalizedRect &endRect = endMatch->constLast();
                    const double startY = (rect.top + rect.bottom) / 2.0;
                    const double endY = (endRect.top + endRect.bottom) / 2.0;
                    const double endX = std::max(endRect.right, rect.left + 0.001);
                    const Okular::TextSelection selection(Okular::NormalizedPoint(rect.left, startY), Okular::NormalizedPoint(endX, endY));
                    std::unique_ptr<Okular::RegularAreaRect> fullArea = page->textArea(selection);
                    if (fullArea && !fullArea->isEmpty()) {
                        m_pageView->setAgentProposalTextHighlight(match.pageNumber, std::move(fullArea));
                        m_document->resetSearch(AgentProposalSearchId);
                    }
                }
            }
            DocumentViewport viewport(match.pageNumber);
            viewport.rePos.enabled = true;
            viewport.rePos.pos = DocumentViewport::TopLeft;
            viewport.rePos.normalizedX = std::max(0.0, rect.left - 0.04);
            viewport.rePos.normalizedY = std::max(0.0, rect.top - 0.10);
            m_document->setViewport(viewport, nullptr, true);
        }
        if (completedSearchTargetsCurrentText) {
            m_pageView->showAgentProposalBuildRequired(completedProposalId);
        }
    } else if (completedProposalId == m_currentAgentProposalReviewId && status == Document::NoMatchFound) {
        if (!completedFallbackText.trimmed().isEmpty() && m_pendingAgentProposalTextSearchId.isEmpty()) {
            QTimer::singleShot(0, this, [this, completedProposalId, completedFallbackText, completedSourcePath, completedSourceLine] {
                if (completedProposalId == m_currentAgentProposalReviewId && !m_agentProposalTextSearchRunning) {
                    startAgentProposalTextSearch(completedProposalId, completedFallbackText, QString(), true, completedSourcePath, completedSourceLine);
                }
            });
            return;
        } else {
            const QVariantMap result = focusAgentProposal(completedProposalId, completedSourcePath, completedSourceLine);
            if (!result.value(QStringLiteral("focused")).toBool() && !result.value(QStringLiteral("pending")).toBool()) {
                m_pageView->showAgentProposalLocationUnavailable(completedProposalId);
            }
        }
    } else if (completedProposalId != m_currentAgentProposalReviewId) {
        m_document->resetSearch(AgentProposalSearchId);
    }

    if (!m_pendingAgentProposalTextSearchId.isEmpty()) {
        const QString pendingId = m_pendingAgentProposalTextSearchId;
        const QString pendingText = m_pendingAgentProposalTextSearchText;
        const QString pendingFallbackText = m_pendingAgentProposalTextSearchFallbackText;
        const QString pendingSourcePath = m_pendingAgentProposalTextSearchSourcePath;
        const int pendingSourceLine = m_pendingAgentProposalTextSearchSourceLine;
        const bool pendingTargetsCurrentText = m_pendingAgentProposalTextSearchTargetsCurrentText;
        m_pendingAgentProposalTextSearchId.clear();
        m_pendingAgentProposalTextSearchText.clear();
        m_pendingAgentProposalTextSearchFallbackText.clear();
        m_pendingAgentProposalTextSearchSourcePath.clear();
        m_pendingAgentProposalTextSearchSourceLine = 0;
        m_pendingAgentProposalTextSearchTargetsCurrentText = false;
        if (pendingId == m_currentAgentProposalReviewId) {
            startAgentProposalTextSearch(pendingId, pendingText, pendingFallbackText, pendingTargetsCurrentText, pendingSourcePath, pendingSourceLine);
        }
    }
}

void Part::requestAgentProposalReview(const QString &proposalId, int attempt)
{
    if (attempt == 0) {
        m_pendingAgentProposalReviewId = proposalId;
    } else if (m_pendingAgentProposalReviewId != proposalId) {
        return;
    }
    const QVariantMap result = openAgentProposalReview(proposalId);
    if (result.value(QStringLiteral("opened")).toBool()) {
        m_pendingAgentProposalReviewId.clear();
        return;
    }
    if (result.value(QStringLiteral("pending")).toBool() && attempt < 40) {
        QTimer::singleShot(50, this, [this, proposalId, attempt] { requestAgentProposalReview(proposalId, attempt + 1); });
        return;
    }
    m_pendingAgentProposalReviewId.clear();
    const QString error = result.value(QStringLiteral("error")).toString();
    errorMessage(error.isEmpty() ? i18n("The proposal review could not be opened.") : error, 10000);
}

void Part::resolveAgentProposalFromCard(const QString &proposalId, const QString &action)
{
    if (!m_pageView || !m_reviewsWidget || (action != QLatin1String("accept") && action != QLatin1String("disregard"))) {
        return;
    }
    const AgentProposalSidebarEntry proposal = m_reviewsWidget->agentProposal(proposalId);
    // Decided from the page's hover toolbar, no window shows the reason: the
    // proposal message bar does.
    const auto refuse = [this, &proposalId, &action](const QString &error) {
        m_pageView->showAgentProposalResolutionError(proposalId, error);
        if (m_pageView->openAgentProposalId() != proposalId) {
            showAgentProposalStatus(action == QLatin1String("accept") ? i18n("The edit was not accepted: %1", error) : i18n("The proposal was not rejected: %1", error),
                                    KMessageWidget::Error,
                                    m_agentProposalUndoState.available && m_pendingLiveProposalDecisions.isEmpty());
        }
    };
    if (proposal.id != proposalId) {
        refuse(i18n("The proposal is no longer available."));
        return;
    }
    if (!proposal.resolutionError.isEmpty()) {
        refuse(proposal.resolutionError);
        return;
    }

    QString error;
    if (proposal.liveSource && m_liveProposalDecisions) {
        queueLiveAgentProposalDecision({proposal.id}, proposal.sourcePath, proposal.sourceSha256, action);
        return;
    } else if (!proposal.localSourcePresent && proposal.bundleId.size() == 64) {
        m_pageView->setAgentProposalOperationPending(proposalId, true, i18n("Applying the proposal decision…"));
        const QString operationId = resolveAgentProposals(proposal.bundleId, action, {proposal.id});
        if (operationId.isEmpty()) {
            error = i18n("The proposal decision could not be written to the durable queue for the server.");
        } else {
            m_pageView->setAgentProposalOperationPending(proposalId, true, i18n("Decision saved locally — the next Sync sends it to the server."));
            showAgentProposalStatus(i18n("Decision saved locally — the next Sync sends it to the server."), KMessageWidget::Information, false);
            return;
        }
    } else {
        error = i18n("The TeX source changed before this proposal could be applied.");
    }
    refuse(error.isEmpty() ? i18n("The proposal could not be resolved.") : error);
}

void Part::queueLiveAgentProposalDecision(const QStringList &proposalIds, const QString &sourcePath, const QString &sourceSha256, const QString &action)
{
    // The decision is final for the user at the click: close the window and
    // drop the proposal's marks from the page and sidebar in this event-loop
    // turn. The pending proposal already shows its replacement in the PDF, so
    // an accepted edit needs no rebuild to look right. The source write
    // follows on the decision queue; a refusal brings everything back.
    PendingLiveProposalDecision pending;
    pending.clock.start();
    pending.proposalIds = proposalIds;
    pending.action = action;
    pending.target = url().isLocalFile() ? QFileInfo(url().toLocalFile()).fileName() : QString();
    // The other half of a move pair is decided with it (§12): hide it too when this document shows it.
    for (const QString &proposalId : proposalIds) {
        if (const QString partner = Thoughts::AgentProposalMove::partner(proposalId); !partner.isEmpty() && !pending.proposalIds.contains(partner))
            pending.proposalIds.append(partner);
    }
    for (const QString &proposalId : std::as_const(pending.proposalIds)) {
        m_pageView->suppressAgentProposal(proposalId);
        m_reviewsWidget->suppressAgentProposal(proposalId);
    }
    pending.hiddenNs = pending.clock.nsecsElapsed();
    Q_EMIT developerEvent(action == QLatin1String("accept") ? QStringLiteral("proposal.accept.started") : QStringLiteral("proposal.reject.started"),
                          {{QStringLiteral("proposal_id"), proposalIds.constFirst()}, {QStringLiteral("proposals"), int(proposalIds.size())}, {QStringLiteral("queued_before"), m_liveProposalDecisions->pending()}});
    // Undo applies to a written decision only.
    setAgentProposalUndoEnabled(false);
    Thoughts::LiveProposalDecisionRequest request;
    request.docsRoot = m_docsRenderRoot;
    request.sourcePath = sourcePath;
    request.expectedSourceSha256 = sourceSha256;
    request.proposalIds = proposalIds;
    request.action = action;
    m_pendingLiveProposalDecisions.insert(m_liveProposalDecisions->enqueue(request), pending);
}

void Part::resolveAgentProposalsFromSidebar(const QStringList &proposalIds, const QString &action)
{
    if (!m_pageView || !m_reviewsWidget || (action != QLatin1String("accept") && action != QLatin1String("disregard"))) {
        return;
    }
    QList<QPair<QPair<QString, QString>, QStringList>> sources;
    QHash<QString, QStringList> bundles;
    int refused = 0;
    QString reason;
    for (const QString &proposalId : proposalIds) {
        const AgentProposalDecisionState state = agentProposalDecisionState(proposalId);
        if (!state.decidable) {
            ++refused;
            reason = state.reason;
            continue;
        }
        const AgentProposalSidebarEntry proposal = m_reviewsWidget->agentProposal(proposalId);
        if (proposal.liveSource && m_liveProposalDecisions) {
            const QPair<QString, QString> source(proposal.sourcePath, proposal.sourceSha256);
            const auto known = std::find_if(sources.begin(), sources.end(), [&source](const auto &entry) { return entry.first == source; });
            if (known != sources.end()) known->second.append(proposalId);
            else sources.append({source, {proposalId}});
        } else {
            bundles[proposal.bundleId].append(proposalId);
        }
    }
    for (const auto &[source, ids] : std::as_const(sources)) queueLiveAgentProposalDecision(ids, source.first, source.second, action);
    for (auto bundle = bundles.cbegin(); bundle != bundles.cend(); ++bundle) {
        if (resolveAgentProposals(bundle.key(), action, bundle.value()).isEmpty()) {
            refused += bundle.value().size();
            reason = i18n("The proposal decision could not be written to the durable queue for the server.");
            continue;
        }
        for (const QString &proposalId : bundle.value()) {
            m_pageView->setAgentProposalOperationPending(proposalId, true, i18n("Decision saved locally — the next Sync sends it to the server."));
        }
        showAgentProposalStatus(i18n("Decision saved locally — the next Sync sends it to the server."), KMessageWidget::Information, false);
    }
    if (refused > 0) {
        showAgentProposalStatus(i18np("%1 proposal was not decided: %2", "%1 proposals were not decided: %2", refused, reason), KMessageWidget::Warning, false);
    }
}

AgentProposalDecisionState Part::agentProposalDecisionState(const QString &proposalId) const
{
    AgentProposalDecisionState state;
    const AgentProposalSidebarEntry proposal = m_reviewsWidget ? m_reviewsWidget->agentProposal(proposalId) : AgentProposalSidebarEntry();
    if (proposal.id != proposalId) {
        state.reason = i18n("The proposal is still being read from its source. Try again in a moment, or open it.");
        return state;
    }
    state.removal = proposal.after.trimmed().isEmpty() && proposal.resolutionError.isEmpty();
    for (const PendingLiveProposalDecision &pending : std::as_const(m_pendingLiveProposalDecisions)) {
        if (pending.proposalIds.contains(proposalId)) {
            state.reason = i18n("A decision on this proposal is being saved.");
            return state;
        }
    }
    for (const PendingAgentProposalResolution &pending : std::as_const(m_pendingAgentProposalResolutions)) {
        if (pending.proposalIds.contains(proposalId)) {
            state.reason = i18n("A decision on this proposal is waiting for Sync.");
            return state;
        }
    }
    if (!proposal.resolutionError.isEmpty()) {
        state.reason = proposal.resolutionError;
        return state;
    }
    // AgentProposalWindow::updateActions(): a live local source, or a Fedora artifact without one.
    const bool canResolveLocally = proposal.liveSource && proposal.sourceSha256.size() == 64;
    const bool canResolveArtifact = !proposal.localSourcePresent && proposal.bundleId.size() == 64;
    if (!canResolveLocally && !canResolveArtifact) {
        state.reason = i18n("The proposal's source is not available here.");
        return state;
    }
    state.decidable = true;
    return state;
}

void Part::finishLiveAgentProposalDecision(const Thoughts::LiveProposalDecisionResult &result)
{
    const auto found = m_pendingLiveProposalDecisions.constFind(result.ticket);
    if (found == m_pendingLiveProposalDecisions.constEnd()) {
        return;
    }
    const PendingLiveProposalDecision pending = found.value();
    m_pendingLiveProposalDecisions.erase(found);
    const bool accept = pending.action == QLatin1String("accept");
    const qint64 durationMs = pending.clock.elapsed();
    QJsonObject metadata {{QStringLiteral("proposal_id"), pending.proposalIds.constFirst()},
                          {QStringLiteral("proposals"), int(pending.proposalIds.size())},
                          {QStringLiteral("ok"), result.ok},
                          {QStringLiteral("duration_ms"), durationMs},
                          {QStringLiteral("hidden_ms"), qRound64(pending.hiddenNs / 1e6)},
                          {QStringLiteral("hidden_us"), pending.hiddenNs / 1000},
                          {QStringLiteral("wait_ms"), result.waitMs},
                          {QStringLiteral("write_ms"), result.workMs}};
    if (!result.ok && !result.errorCode.isEmpty()) {
        metadata.insert(QStringLiteral("error_code"), result.errorCode);
    }
    Q_EMIT developerEvent(accept ? QStringLiteral("proposal.accept.finished") : QStringLiteral("proposal.reject.finished"), metadata);

    if (!result.ok) {
        const QString error = result.error.isEmpty() ? i18n("The proposal could not be resolved.") : result.error;
        // Nothing was written: bring the proposal back where the user left it.
        restoreAgentProposalSurfaces(pending.proposalIds, false);
        const QString openProposal = m_pageView ? m_pageView->openAgentProposalId() : QString();
        const QString first = pending.proposalIds.constFirst();
        if (m_pageView && (openProposal.isEmpty() || pending.proposalIds.contains(openProposal))
            && openAgentProposalReview(first).value(QStringLiteral("opened")).toBool()) {
            m_pageView->showAgentProposalResolutionError(first, error);
        }
        showAgentProposalStatus(accept ? i18n("The edit was not accepted: %1", error) : i18n("The proposal was not rejected: %1", error),
                                KMessageWidget::Error,
                                m_agentProposalUndoState.available && m_pendingLiveProposalDecisions.isEmpty());
        return;
    }

    m_agentProposalUndoState = {true,
                                false,
                                {},
                                pending.proposalIds,
                                pending.action,
                                result.request.sourcePath,
                                result.updatedSourceSha256,
                                result.previousContents,
                                pending.target};
    m_agentProposalUndoState.otherSources = result.otherSources;
    if (m_pendingLiveProposalDecisions.isEmpty()) {
        hideAgentProposalStatus(true);
        if (m_pageView) {
            const int count = pending.proposalIds.size();
            m_pageView->displayMessage(accept ? i18np("Accepted — saved to TeX. No PDF rebuild needed.", "%1 proposals accepted — saved to TeX. No PDF rebuild needed.", count)
                                              : i18np("Rejected — saved to TeX. Sync to update the PDF.", "%1 proposals rejected — saved to TeX. Sync to update the PDF.", count),
                                       QString(),
                                       PageViewMessage::Info,
                                       2500);
        }
    }
}

void Part::handleAgentProposalResolutionStatus(const QString &operationId, const QString &status, const QString &message)
{
    const auto found = m_pendingAgentProposalResolutions.constFind(operationId);
    if (found == m_pendingAgentProposalResolutions.constEnd()) {
        return;
    }
    const PendingAgentProposalResolution pending = found.value();
    if (status == QLatin1String("waiting")) {
        if (!pending.undo) {
            for (const QString &proposalId : pending.proposalIds) {
                if (m_pageView) {
                    m_pageView->setAgentProposalOperationPending(proposalId, true, i18n("Decision saved locally — the next Sync sends it to the server."));
                }
            }
        }
        showAgentProposalStatus(i18n("Decision saved locally — the next Sync sends it to the server."), KMessageWidget::Information, false);
        Q_EMIT agentProposalResolutionStatusChanged(operationId,
                                                    pending.proposalIds,
                                                    pending.undo ? QStringLiteral("undo_") + pending.action : pending.action,
                                                    status,
                                                    message);
        return;
    }
    if (status == QLatin1String("failed")) {
        const QString error = message.isEmpty() ? i18n("The server could not apply the proposal decision.") : message;
        if (pending.undo) {
            showAgentProposalStatus(error, KMessageWidget::Error, m_agentProposalUndoState.available);
        } else {
            for (const QString &proposalId : pending.proposalIds) {
                if (m_pageView) {
                    m_pageView->showAgentProposalResolutionError(proposalId, error);
                }
            }
            showAgentProposalStatus(error, KMessageWidget::Error, false);
        }
        m_pendingAgentProposalResolutions.remove(operationId);
        Q_EMIT agentProposalResolutionStatusChanged(operationId,
                                                    pending.proposalIds,
                                                    pending.undo ? QStringLiteral("undo_") + pending.action : pending.action,
                                                    status,
                                                    error);
        return;
    }
    if (status != QLatin1String("applied")) {
        return;
    }
    if (pending.undo) {
        const QString success = pending.action == QLatin1String("accept")
            ? i18n("Acceptance undone — proposal restored. No PDF rebuild needed.")
            : i18n("Rejection undone — proposal restored. PDF update queued.");
        restoreAgentProposalSurfaces(pending.proposalIds);
        m_agentProposalUndoState = {};
        hideAgentProposalStatus(false);
        Q_EMIT agentProposalResolutionStatusChanged(operationId,
                                                    pending.proposalIds,
                                                    QStringLiteral("undo_") + pending.action,
                                                    status,
                                                    success);
        m_pendingAgentProposalResolutions.remove(operationId);
        return;
    }
    const QString success = pending.action == QLatin1String("accept")
        ? i18n("Accepted — saved to TeX. No PDF rebuild needed.")
        : i18n("Rejected — saved to TeX. PDF update queued.");
    for (const QString &proposalId : pending.proposalIds) {
        if (m_pageView) {
            m_pageView->showAgentProposalResolutionSuccess(proposalId, success);
        }
    }
    m_agentProposalUndoState = {true,
                                true,
                                operationId,
                                pending.proposalIds,
                                pending.action,
                                {},
                                {},
                                {},
                                url().isLocalFile() ? QFileInfo(url().toLocalFile()).fileName() : QString()};
    hideAgentProposalStatus(true);
    Q_EMIT agentProposalResolutionStatusChanged(operationId, pending.proposalIds, pending.action, status, success);
    m_pendingAgentProposalResolutions.remove(operationId);
    QTimer::singleShot(650, this, [this, proposalIds = pending.proposalIds] {
        for (const QString &proposalId : proposalIds) {
            if (m_reviewsWidget) {
                m_reviewsWidget->suppressAgentProposal(proposalId);
            }
            if (m_pageView) {
                m_pageView->suppressAgentProposal(proposalId);
            }
        }
    });
}

void Part::showAgentProposalStatus(const QString &message, KMessageWidget::MessageType messageType, bool undoEnabled)
{
    setAgentProposalUndoEnabled(undoEnabled);
    if (!m_agentProposalMessage) {
        return;
    }
    m_agentProposalMessage->setText(message);
    m_agentProposalMessage->setMessageType(messageType);
    m_agentProposalMessage->setVisible(true);
}

void Part::hideAgentProposalStatus(bool undoEnabled)
{
    setAgentProposalUndoEnabled(undoEnabled);
    if (m_agentProposalMessage) {
        m_agentProposalMessage->animatedHide();
    }
}

void Part::setAgentProposalUndoEnabled(bool enabled)
{
    m_agentProposalUndoEnabled = enabled && m_agentProposalUndoState.available;
    updateUndoAction();
}

void Part::updateUndoAction()
{
    if (!m_undoAction) {
        return;
    }
    const bool commentUndo = !m_commentUndoTokens.isEmpty() && m_commentHistoryDocumentIndex.value(m_commentUndoTokens.last()) == m_document->undoHistoryIndex();
    m_undoAction->setEnabled((m_pageView && m_pageView->canUndoAnnotation())
                             || (m_agentProposalUndoState.available ? m_agentProposalUndoEnabled : commentUndo || m_document->canUndo()));
    if (auto *redo = actionCollection()->action(QStringLiteral("edit_redo"))) redo->setEnabled(!m_commentRedoTokens.isEmpty() || m_document->canRedo());
}

void Part::restoreAgentProposalSurfaces(const QStringList &proposalIds, bool reloadSources)
{
    for (const QString &proposalId : proposalIds) {
        if (m_reviewsWidget) {
            m_reviewsWidget->restoreAgentProposal(proposalId, reloadSources);
        }
        if (m_pageView) {
            m_pageView->restoreAgentProposal(proposalId);
        }
    }
}

void Part::undoLastAgentProposalResolution()
{
    if (!m_agentProposalUndoState.available) {
        return;
    }
    const AgentProposalUndoState undo = m_agentProposalUndoState;
    if (undo.remote) {
        if (!m_fedoraRepository) {
            showAgentProposalStatus(i18n("The server is unavailable."), KMessageWidget::Error, true);
            return;
        }
        const QString undoOperationId = m_fedoraRepository->undoAgentProposalResolution(undo.resolutionId);
        if (undoOperationId.isEmpty()) {
            showAgentProposalStatus(i18n("The proposal Undo could not be written to the durable queue for the server."), KMessageWidget::Error, true);
            return;
        }
        m_pendingAgentProposalResolutions.insert(undoOperationId, {undo.proposalIds, undo.action, true});
        showAgentProposalStatus(i18n("Decision saved locally — the next Sync sends it to the server."), KMessageWidget::Information, false);
        return;
    }

    QString error;
    if (!Thoughts::restoreLiveAgentProposalSources(
            m_docsRenderRoot,
            undo.sourcePath,
            undo.expectedSourceSha256,
            undo.previousContents,
            undo.otherSources,
            &error)) {
        showAgentProposalStatus(error.isEmpty() ? i18n("The proposal decision could not be undone safely.") : error, KMessageWidget::Error, true);
        return;
    }
    restoreAgentProposalSurfaces(undo.proposalIds);
    m_agentProposalUndoState = {};
    const QString success = undo.action == QLatin1String("accept")
        ? i18n("Acceptance undone — proposal restored. No PDF rebuild needed.")
        : i18n("Rejection undone — proposal restored. The next Sync updates the PDF.");
    hideAgentProposalStatus(false);
    Q_EMIT agentProposalResolutionStatusChanged({},
                                                undo.proposalIds,
                                                QStringLiteral("undo_") + undo.action,
                                                QStringLiteral("applied"),
                                                success);
}

void Part::focusSidebarAgentProposal(const QString &proposalId, const QString &sourcePath, int sourceLine, int attempt)
{
    Q_UNUSED(sourcePath)
    Q_UNUSED(sourceLine)
    Q_UNUSED(attempt)
    requestAgentProposalReview(proposalId);
}

QVariantMap Part::fedoraManualSyncState() const
{
    return m_fedoraRepository
        ? QVariantMap {{QStringLiteral("state"), static_cast<int>(m_fedoraRepository->manualSyncState())},
                       {QStringLiteral("target"), m_fedoraRepository->manualSyncTarget()},
                       {QStringLiteral("operation_id"), m_fedoraRepository->manualSyncOperationId()},
                       {QStringLiteral("message"), m_fedoraRepository->manualSyncMessage()},
                       {QStringLiteral("details"), m_fedoraRepository->manualSyncDetails()},
                       {QStringLiteral("error_code"), m_fedoraRepository->manualSyncErrorCode()},
                       {QStringLiteral("failure_kind"), m_fedoraRepository->manualSyncFailureKind()},
                       {QStringLiteral("warning_code"), m_fedoraRepository->manualSyncWarningCode()},
                       {QStringLiteral("warning"), m_fedoraRepository->manualSyncWarning()}}
        : QVariantMap {{QStringLiteral("state"), static_cast<int>(FedoraRepository::ManualSyncState::Failed)},
                       {QStringLiteral("message"), QStringLiteral("Sync with the server is unavailable.")},
                       {QStringLiteral("details"), QString()}};
}

void Part::checkFedoraRemoteWork()
{
    if (!m_fedoraRepository) {
        Q_EMIT fedoraRemoteWorkChecked(false, QString());
        return;
    }
    QPointer<Part> self(this);
    m_fedoraRepository->checkRemoteWork([self](bool pending, const QString &stateKey) {
        if (self) {
            Q_EMIT self->fedoraRemoteWorkChecked(pending, stateKey);
        }
    });
}

void Part::requestFedoraBuildForTarget(const QString &target, const QString &traceId)
{
    if (!m_fedoraRepository || target.isEmpty()) {
        return;
    }
    m_fedoraRepository->captureDocument(QFileInfo(target).completeBaseName());
    m_fedoraRepository->publishSources(QFileInfo(target).fileName(), traceId);
}

void Part::watchFedoraTrace(const QString &traceId, qint64 afterSequence)
{
    if (m_fedoraRepository) {
        m_fedoraRepository->watchTrace(traceId, afterSequence);
    }
}

void Part::stopWatchingFedoraTrace(const QString &traceId)
{
    if (m_fedoraRepository) {
        m_fedoraRepository->stopWatchingTrace(traceId);
    }
}

void Part::requestFedoraRenderStatus(const QString &target)
{
    if (m_fedoraRepository) {
        m_fedoraRepository->refreshRenderStatus(target);
    }
}

void Part::requestFedoraRenderStateAfterExplicitOperation()
{
    if (m_fedoraRepository) {
        m_fedoraRepository->refreshRenderStateAfterExplicitOperation();
    }
}

void Part::requestFedoraBuildInputSnapshot(const QString &target)
{
    if (m_fedoraRepository) {
        m_fedoraRepository->refreshLatestBuildInputs(target);
    }
}

QVariantMap Part::desktopSourceMonitoringInformation(const QString &target)
{
    return m_fedoraRepository
        ? m_fedoraRepository->sourceMonitoringInformation(target).toVariantMap()
        : QVariantMap {
              {QStringLiteral("schema_version"), 1},
              {QStringLiteral("target"), target},
              {QStringLiteral("status"), QStringLiteral("unavailable")},
              {QStringLiteral("error"), QStringLiteral("Local source monitoring is unavailable.")},
          };
}

void Part::requestDesktopSourceMonitoringInformation(const QStringList &targets, QObject *receiver, quint64 requestId)
{
    const QPointer<QObject> guard(receiver);
    const auto deliver = [guard, requestId](const QVariantMap &information) {
        if (guard) {
            QMetaObject::invokeMethod(guard.data(), "applySourceMonitoringBatch", Qt::QueuedConnection,
                                      Q_ARG(quint64, requestId), Q_ARG(QVariantMap, information));
        }
    };
    if (m_fedoraRepository) {
        m_fedoraRepository->requestSourceMonitoringInformation(targets, deliver);
    } else {
        deliver({});
    }
}

void Part::requestFedoraLibraryActivation(const QString &target)
{
    if (m_fedoraRepository) {
        m_fedoraRepository->activatePublishedPdf(target);
    }
}

void Part::publishNewSourceDocument(const QString &project, const QString &target, const QString &traceId)
{
    if (m_fedoraRepository) {
        m_fedoraRepository->publishNewSourceDocument(project, target, traceId);
    }
}

QVariantMap Part::fedoraBuildActivity(const QString &target) const
{
    return m_fedoraRepository ? m_fedoraRepository->buildActivity(target).toVariantMap() : QVariantMap();
}

QVariantMap Part::fedoraSourcePublicationActivity() const
{
    return m_fedoraRepository ? m_fedoraRepository->sourcePublicationActivity().toVariantMap() : QVariantMap();
}

QVariantMap Part::sourceTrackingDiagnostics() const
{
    return m_fedoraRepository ? m_fedoraRepository->sourceTrackingDiagnostics() : QVariantMap();
}

QVariantMap Part::fedoraRenderStatus(const QString &target)
{
    return m_fedoraRepository ? m_fedoraRepository->renderStatus(target).toVariantMap() : QVariantMap();
}

QVariantMap Part::fedoraRenderState() const
{
    return m_fedoraRepository ? m_fedoraRepository->renderState().toVariantMap() : QVariantMap();
}

void Part::importLibraryPdf(const QString &requestId, const QString &sourcePath, const QString &filename)
{
    if (m_fedoraRepository) {
        m_fedoraRepository->importLibraryPdf(requestId, sourcePath, filename);
    }
}

void Part::setLibraryDisplayName(const QString &requestId, const QString &filename, const QString &expectedSha256, const QString &displayName)
{
    if (m_fedoraRepository) {
        m_fedoraRepository->setLibraryDisplayName(requestId, filename, expectedSha256, displayName);
    }
}

QString Part::localDocumentPath() const
{
    return localFilePath();
}

void Part::removeLibraryPdf(const QString &requestId, const QString &filename, const QString &expectedSha256)
{
    if (m_fedoraRepository) {
        m_fedoraRepository->removeLibraryPdf(requestId, filename, expectedSha256);
    }
}

void Part::slotPrint()
{
    if (m_document->pages() == 0) {
        return;
    }

#ifdef Q_OS_WIN
    QPrinter printer(QPrinter::HighResolution);
#else
    QPrinter printer;
#endif
    QWidget *printConfigWidget = nullptr;

    // Must do certain QPrinter setup before creating QPrintDialog
    setupPrint(printer);

    // Create the Print Dialog with extra config widgets if required
    if (m_document->canConfigurePrinter()) {
        printConfigWidget = m_document->printConfigurationWidget();
    } else {
        printConfigWidget = new DefaultPrintOptionsWidget();
    }

    QPrintDialog printDialog(&printer, widget());
    printDialog.setWindowTitle(i18nc("@title:window", "Print"));
    QList<QWidget *> options;
    if (printConfigWidget) {
        options << printConfigWidget;
    }
    printDialog.setOptionTabs(options);

    // Set the available Print Range
    printDialog.setMinMax(1, m_document->pages());
    printDialog.setFromTo(1, m_document->pages());

    // If the user has bookmarked pages for printing, then enable Selection
    if (!m_document->bookmarkedPageRange().isEmpty()) {
        printDialog.setOption(QAbstractPrintDialog::PrintSelection);
    }

    // If the Document type doesn't support print to both PS & PDF then disable the Print Dialog option
    if (printDialog.testOption(QAbstractPrintDialog::PrintToFile) && !m_document->supportsPrintToFile()) {
        printDialog.setOption(QAbstractPrintDialog::PrintToFile, false);
    }

    // Enable the Current Page option in the dialog.
    if (m_document->pages() > 1 && currentPage() > 0) {
        printDialog.setOption(QAbstractPrintDialog::PrintCurrentPage);
    }

    bool success = true;
    if (printDialog.exec()) {
        // set option for margins if widget is of corresponding type that holds this information
        PrintOptionsWidget *optionWidget = dynamic_cast<PrintOptionsWidget *>(printConfigWidget);
        if (optionWidget != nullptr) {
            printer.setFullPage(optionWidget->ignorePrintMargins());
        } else {
            // printConfigurationWidget() method should always return an object of type Okular::PrintOptionsWidget,
            // (signature does not (yet) require it for ABI stability reasons), so Q_EMIT a warning if the object is of another type
            qWarning() << "printConfigurationWidget() method did not return an Okular::PrintOptionsWidget. This is strongly discouraged!";
        }

        success = doPrint(printer);
    }

    if (m_cliPrintAndExit) {
        exit(success ? EXIT_SUCCESS : EXIT_FAILURE);
    }
}

void Part::setupPrint(QPrinter &printer)
{
    printer.setPageOrientation(m_document->orientation());

    // title
    QString title = m_document->metaData(QStringLiteral("DocumentTitle")).toString();
    if (title.isEmpty()) {
        title = m_document->currentDocument().fileName();
    }
    if (!title.isEmpty()) {
        printer.setDocName(title);
    }
}

bool Part::doPrint(QPrinter &printer)
{
    if (!m_document->isAllowed(Okular::AllowPrint)) {
        KMessageBox::error(widget(), i18n("Printing this document is not allowed."));
        return false;
    }

    if (printer.printRange() == QPrinter::CurrentPage) {
        printer.setPrintRange(QPrinter::PageRange);
        printer.setFromTo(currentPage(), currentPage());
    }

    const Document::PrintError printError = m_document->print(printer);
    if (printError != Document::NoPrintError) {
        const QString error = Okular::Document::printErrorString(printError);
        if (error.isEmpty()) {
            KMessageBox::error(widget(), i18n("Could not print the document. Unknown error. Please report to bugs.kde.org"));
        } else {
            KMessageBox::error(widget(), i18n("Could not print the document. Detailed error is \"%1\". Please report to bugs.kde.org", error));
        }
        return false;
    }
    return true;
}

void Part::psTransformEnded(int exit, QProcess::ExitStatus status)
{
    Q_UNUSED(exit)
    if (status != QProcess::NormalExit) {
        return;
    }

    QProcess *senderobj = sender() ? qobject_cast<QProcess *>(sender()) : nullptr;
    if (senderobj) {
        senderobj->close();
        senderobj->deleteLater();
    }

    setLocalFilePath(m_temporaryLocalFile);
    openUrl(QUrl::fromLocalFile(m_temporaryLocalFile));
    m_temporaryLocalFile.clear();
}

void Part::displayInfoMessage(const QString &message, KMessageWidget::MessageType messageType, int duration)
{
    if (!Okular::Settings::showOSD()) {
        if (messageType == KMessageWidget::Error) {
            KMessageBox::error(widget(), message);
        }
        return;
    }

    // hide messageWindow if string is empty
    if (message.isEmpty()) {
        m_infoMessage->animatedHide();
    }

    // display message (duration is length dependent)
    if (duration < 0) {
        duration = 500 + 100 * message.length();
    }
    m_infoTimer->start(duration);
    m_infoMessage->setText(message);
    m_infoMessage->setMessageType(messageType);
    m_infoMessage->setVisible(true);
}

void Part::errorMessage(const QString &message, int duration)
{
    displayInfoMessage(message, KMessageWidget::Error, duration);
}

void Part::warningMessage(const QString &message, int duration)
{
    displayInfoMessage(message, KMessageWidget::Warning, duration);
}

void Part::noticeMessage(const QString &message, int duration)
{
    // less important message -> simpler display widget in the PageView
    m_pageView->displayMessage(message, QString(), PageViewMessage::Info, duration);
}

void Part::moveSplitter(int sideWidgetSize)
{
    m_sidebar->moveSplitter(sideWidgetSize);
}

#if HAVE_NEW_SIGNATURE_API
void Part::finishSigning()
{
    if (m_pageView->finishSigning() != PageView::FinishSigningResult::Cancelled) {
        m_signatureInProgressMessage->setVisible(false);
        if (QAction *signatureAction = actionCollection()->action(QStringLiteral("add_digital_signature"))) {
            signatureAction->setEnabled(false);
            signatureAction->setVisible(false);
        }
    }
}
#endif

void Part::unsetDummyMode()
{
    if (m_embedMode == PrintPreviewMode) {
        return;
    }

    m_sidebar->setSidebarVisibility(Okular::Settings::showLeftPanel());

    // add back and next in history
    m_historyBack = KStandardAction::documentBack(this, SLOT(slotHistoryBack()), actionCollection());
    m_historyBack->setWhatsThis(i18n("Go to the place you were before"));
    // Cmd+[ and Cmd+] on the Mac, as in Safari and Finder, besides the standard shortcuts.
    actionCollection()->setDefaultShortcuts(m_historyBack, m_historyBack->shortcuts() << QKeySequence(Qt::CTRL | Qt::Key_BracketLeft));
    connect(m_pageView.data(), &PageView::mouseBackButtonClick, m_historyBack, &QAction::trigger);

    m_historyNext = KStandardAction::documentForward(this, SLOT(slotHistoryNext()), actionCollection());
    m_historyNext->setWhatsThis(i18n("Go to the place you were after"));
    actionCollection()->setDefaultShortcuts(m_historyNext, m_historyNext->shortcuts() << QKeySequence(Qt::CTRL | Qt::Key_BracketRight));
    connect(m_pageView.data(), &PageView::mouseForwardButtonClick, m_historyNext, &QAction::trigger);

    m_pageView->setupActions(actionCollection());
    m_undoAction = actionCollection()->action(QStringLiteral("edit_undo"));
    if (m_undoAction) {
        disconnect(m_undoAction, nullptr, m_document, nullptr);
        disconnect(m_document, nullptr, m_undoAction, nullptr);
        connect(m_undoAction, &QAction::triggered, this, [this] {
            // The user's newest annotation (a stroke, a text box, a like ...) when nothing undoable
            // came after it.
            if (m_pageView->canUndoAnnotation()
                && (!m_agentProposalUndoState.available || m_pageView->lastAnnotationAt() >= m_agentProposalUndoState.at)) {
                m_pageView->undoLastAnnotation();
            } else if (m_agentProposalUndoState.available) {
                undoLastAgentProposalResolution();
            } else if (!m_commentUndoTokens.isEmpty() && m_commentHistoryDocumentIndex.value(m_commentUndoTokens.last()) == m_document->undoHistoryIndex()) {
                const auto result = undoCommentDeletion(m_commentUndoTokens.last());
                if (!result.value(QStringLiteral("success")).toBool()) KMessageBox::error(widget(), result.value(QStringLiteral("error")).toString());
            } else {
                m_document->undo();
            }
        });
        connect(m_document, &Okular::Document::canUndoChanged, this, [this] {
            updateUndoAction();
        });
        connect(m_pageView.data(), &PageView::annotationUndoChanged, this, [this] {
            updateUndoAction();
        });
#ifdef Q_OS_MACOS
        // The Control key too, as on other systems.
        actionCollection()->setDefaultShortcuts(m_undoAction, m_undoAction->shortcuts() << QKeySequence(Qt::META | Qt::Key_Z));
#endif
        updateUndoAction();
    }

    if (auto *redo = actionCollection()->action(QStringLiteral("edit_redo"))) {
        disconnect(redo, nullptr, m_document, nullptr);
        connect(redo, &QAction::triggered, this, [this] {
            if (!m_commentRedoTokens.isEmpty() && m_commentHistoryDocumentIndex.value(m_commentRedoTokens.last()) == m_document->undoHistoryIndex()) redoCommentDeletion(m_commentRedoTokens.last());
            else m_document->redo();
        });
    }

    // attach the actions of the children widgets too
    m_formsMessage->addAction(m_pageView->toggleFormsAction());

    // ensure history actions are in the correct state
    updateViewActions();
}

bool Part::handleCompressed(QString &destpath, const QString &path, KCompressionDevice::CompressionType compressionType)
{
    m_tempfile = nullptr;

    // we are working with a compressed file, decompressing
    // temporary file for decompressing
    QTemporaryFile *newtempfile = new QTemporaryFile();
    newtempfile->setAutoRemove(true);

    if (!newtempfile->open()) {
        KMessageBox::error(widget(),
                           i18n("<qt><strong>File Error!</strong> Could not create temporary file "
                                "<nobr><strong>%1</strong></nobr>.</qt>",
                                newtempfile->errorString()));
        delete newtempfile;
        return false;
    }

    // decompression filer
    KCompressionDevice dev(path, compressionType);

    if (!dev.open(QIODevice::ReadOnly)) {
        KMessageBox::detailedError(widget(),
                                   i18n("<qt><strong>File Error!</strong> Could not open the file "
                                        "<nobr><strong>%1</strong></nobr> for uncompression. "
                                        "The file will not be loaded.</qt>",
                                        path),
                                   i18n("<qt>This error typically occurs if you do "
                                        "not have enough permissions to read the file. "
                                        "You can check ownership and permissions if you "
                                        "right-click on the file in the Dolphin "
                                        "file manager, then choose the 'Properties' option, "
                                        "and select 'Permissions' tab in the opened window.</qt>"));

        delete newtempfile;
        return false;
    }

    char buf[65536];
    int read = 0;

    while ((read = dev.read(buf, sizeof(buf))) > 0) {
        int wrtn = newtempfile->write(buf, read);
        if (read != wrtn) {
            break;
        }
    }
    if ((read != 0) || (newtempfile->size() == 0)) {
        KMessageBox::detailedError(widget(),
                                   i18n("<qt><strong>File Error!</strong> Could not uncompress "
                                        "the file <nobr><strong>%1</strong></nobr>. "
                                        "The file will not be loaded.</qt>",
                                        path),
                                   i18n("<qt>This error typically occurs if the file is corrupt. "
                                        "If you want to be sure, try to decompress the file manually "
                                        "using command-line tools.</qt>"));
        delete newtempfile;
        return false;
    }
    m_tempfile = newtempfile;
    destpath = m_tempfile->fileName();
    return true;
}

void Part::rebuildBookmarkMenu(bool unplugActions)
{
    if (unplugActions) {
        unplugActionList(QStringLiteral("bookmarks_currentdocument"));
        qDeleteAll(m_bookmarkActions);
        m_bookmarkActions.clear();
    }
    QUrl u = m_document->currentDocument();
    if (u.isValid()) {
        m_bookmarkActions = m_document->bookmarkManager()->actionsForUrl(u);
    }
    bool havebookmarks = true;
    if (m_bookmarkActions.isEmpty()) {
        havebookmarks = false;
        QAction *a = new QAction(nullptr);
        a->setText(i18n("No Bookmarks"));
        a->setEnabled(false);
        m_bookmarkActions.append(a);
    }
    plugActionList(QStringLiteral("bookmarks_currentdocument"), m_bookmarkActions);

    if (factory()) {
        const QList<KXMLGUIClient *> clients(factory()->clients());
        bool containerFound = false;
        for (int i = 0; !containerFound && i < clients.size(); ++i) {
            QMenu *container = dynamic_cast<QMenu *>(factory()->container(QStringLiteral("bookmarks"), clients[i]));
            if (container && container->actions().contains(m_bookmarkActions.first())) {
                container->installEventFilter(this);
                containerFound = true;
            }
        }
    }

    m_prevBookmark->setEnabled(havebookmarks);
    m_nextBookmark->setEnabled(havebookmarks);
}

bool Part::eventFilter(QObject *watched, QEvent *event)
{
    switch (event->type()) {
    case QEvent::ContextMenu: {
        QContextMenuEvent *e = static_cast<QContextMenuEvent *>(event);
        QMenu *menu = static_cast<QMenu *>(watched);

        QScopedPointer<QMenu> ctxMenu(new QMenu);

        QPoint pos;
        bool ret = false;
        if (e->reason() == QContextMenuEvent::Mouse) {
            pos = e->pos();
            ret = aboutToShowContextMenu(menu, menu->actionAt(e->pos()), ctxMenu.data());
        } else if (menu->activeAction()) {
            pos = menu->actionGeometry(menu->activeAction()).center();
            ret = aboutToShowContextMenu(menu, menu->activeAction(), ctxMenu.data());
        }
        ctxMenu->exec(menu->mapToGlobal(pos));

        if (ret) {
            event->accept();
        }
        return ret;
    }

    default:
        break;
    }

    return KParts::ReadWritePart::eventFilter(watched, event);
}

void Part::updateAboutBackendAction()
{
    const KPluginMetaData data = m_document->generatorInfo();
    m_aboutBackend->setEnabled(data.isValid());
}

void Part::resetStartArguments()
{
    m_cliPrint = false;
    m_cliPrintAndExit = false;
}

#if HAVE_PURPOSE
void Part::slotShareActionFinished(const QJsonObject &output, int error, const QString &message)
{
    if (error) {
        KMessageBox::error(widget(), i18n("There was a problem sharing the document: %1", message), i18n("Share"));
    } else {
        const QString url = output[QStringLiteral("url")].toString();
        if (url.isEmpty()) {
            m_pageView->displayMessage(i18n("Document shared successfully"));
        } else {
            KMessageBox::information(widget(), i18n("You can find the shared document at: <a href=\"%1\">%1</a>", url), i18n("Share"), QString(), KMessageBox::Notify | KMessageBox::AllowLink);
        }
    }
}
#endif

void Part::setReadWrite(bool readwrite)
{
    Q_UNUSED(readwrite)
    m_document->setAnnotationEditingEnabled(shouldSyncDocsComments(url()) || isWorkspaceThirdPartyPdf(url()));
    ReadWritePart::setReadWrite(false);
}

void Part::enableStartWithFind(const QString &text)
{
    m_textToFindOnOpen = QString(text);
}

void Part::showLibrarySearch(const QString &text, int page)
{
    const QString query = text.trimmed();
    if (query.isEmpty()) {
        return;
    }
    slotShowFindBar();
    m_findBar->startLibrarySearch(query, page - 1);
}

void Part::setEditorCmd(const QString &editorCmd)
{
    m_document->setEditorCommandOverride(editorCmd);
}

void Part::slotOpenContainingFolder()
{
    KIO::highlightInFileManager({QUrl(localFilePath())});
}

void Part::slotCopyAnnotation()
{
    Okular::Annotation *annotation = m_pageView->focusedAnnotation();
    if (!annotation || annotation->subType() != Okular::Annotation::AText) {
        return;
    }

    QDomDocument document(QStringLiteral("okular-annotations"));
    QDomElement root = document.createElement(QStringLiteral("annotations"));
    root.setAttribute(QStringLiteral("version"), AnnotationPopup::annotationClipboardFormatVersion);
    document.appendChild(root);

    QDomElement annotationElement = document.createElement(QStringLiteral("annotation"));
    Okular::AnnotationUtils::storeAnnotation(annotation, annotationElement, document);
    root.appendChild(annotationElement);

    auto *mimeData = new QMimeData();
    mimeData->setData(QLatin1String(AnnotationPopup::annotationClipboardMimeType), document.toByteArray());
    QApplication::clipboard()->setMimeData(mimeData, QClipboard::Clipboard);
}

void Part::slotCopyTextSelectionOrAnnotation()
{
    // The native menu remains available when a separate review window has
    // focus. Copy from its text field before considering the underlying PDF.
    QWidget *focused = QApplication::focusWidget();
    if (auto *text = qobject_cast<QPlainTextEdit *>(focused)) {
        text->copy();
        return;
    }
    if (auto *text = qobject_cast<QTextEdit *>(focused)) {
        text->copy();
        return;
    }
    if (auto *text = qobject_cast<QLineEdit *>(focused)) {
        text->copy();
        return;
    }
    // An HTML attachment: ⌘C is Qt's, so copy the page's selection (never masked text).
    if (m_attachmentShown && m_attachmentView && m_attachmentReader->isReady()) {
        m_attachmentView->call(QStringLiteral("copySelection"), {}, [](bool ok, const QJsonValue &result, const QString &) {
            if (ok && !result.toString().isEmpty()) QGuiApplication::clipboard()->setText(result.toString());
        });
        return;
    }
    // The HTML view: ⌘C is Qt's, so copy the page's selection (never masked text).
    if (htmlViewActive() && m_htmlView && m_htmlReader->isEditionShown()) {
        m_htmlView->call(QStringLiteral("copySelection"), {}, [](bool ok, const QJsonValue &result, const QString &) {
            if (ok && !result.toString().isEmpty()) QGuiApplication::clipboard()->setText(result.toString());
        });
        return;
    }
    Okular::Annotation *annotation = m_pageView->focusedAnnotation();
    if (annotation && annotation->subType() == Okular::Annotation::AText) {
        slotCopyAnnotation();
        return;
    }

    m_pageView->copyTextSelection(PageView::TextCopyMode::AsProvided);
}

void Part::slotPasteAnnotation()
{
    // Annotation paste is intentionally unavailable in Thoughts. Comments are
    // created only through the sidecar-backed pop-up-note and text-highlight paths.
}

QAbstractItemModel *Part::annotationsModel() const
{
    return m_reviewsWidget ? m_reviewsWidget->annotationsModel() : nullptr;
}

QList<QJsonObject> Part::unplacedCommentThread(const QString &annotationId) const
{
    QHash<QString, QJsonObject> records;
    const auto sidecar = DocsCommentSynchronizer::visibleSidecarComments(m_docsRenderRoot, QFileInfo(localFilePath()).completeBaseName(), nullptr, nullptr);
    for (const auto &record : sidecar) records.insert(record.value(QStringLiteral("id")).toString(), record);
    const auto projections = effectiveFedoraProjections();
    for (auto it = projections.begin(); it != projections.end(); ++it) {
        if (!it.key().startsWith(QLatin1String("logical_comment\ncomments\n"))) continue;
        const auto logical = it.value().toObject();
        const QString id = logical.value(QStringLiteral("comment_id")).toString();
        auto record = records.value(id);
        const auto localDate = DocsCommentSynchronizer::parseCommentDate(record.value(QStringLiteral("updated_at")));
        const auto logicalDate = DocsCommentSynchronizer::parseCommentDate(logical.value(QStringLiteral("updated_at")));
        if (localDate.isValid() && logicalDate.isValid() && localDate > logicalDate) continue;
        for (const auto &field : {QStringLiteral("contents"), QStringLiteral("author"), QStringLiteral("subtype"), QStringLiteral("deleted")}) record.insert(field, logical.value(field));
        record.insert(QStringLiteral("id"), id);
        record.insert(QStringLiteral("in_reply_to"), logical.value(QStringLiteral("parent_id")));
        record.insert(QStringLiteral("created"), logical.value(QStringLiteral("created_at")));
        record.insert(QStringLiteral("modified"), logical.value(QStringLiteral("updated_at")));
        records.insert(id, record);
    }
    if (!records.contains(annotationId)) records.insert(annotationId, currentCommentDetailsRecord(annotationId));
    QString root = annotationId;
    QSet<QString> ancestors;
    while (!ancestors.contains(root)) {
        ancestors.insert(root);
        const auto parent = records.value(root).value(QStringLiteral("in_reply_to")).toString();
        if (parent.isEmpty() || !records.contains(parent) || ancestors.contains(parent)) break;
        root = parent;
    }
    QList<QJsonObject> thread;
    QSet<QString> visited;
    std::function<void(const QString &)> append = [&](const QString &id) {
        if (visited.contains(id)) return;
        visited.insert(id);
        const auto record = records.value(id);
        if (!record.isEmpty() && !record.value(QStringLiteral("deleted")).toBool()) thread.append(record);
        QStringList children;
        for (auto it = records.begin(); it != records.end(); ++it) {
            if (it.value().value(QStringLiteral("in_reply_to")).toString() == id) children.append(it.key());
        }
        std::sort(children.begin(), children.end(), [&](const QString &a, const QString &b) {
            const QString left = records.value(a).value(QStringLiteral("created")).toString();
            const QString right = records.value(b).value(QStringLiteral("created")).toString();
            return left == right ? a < b : left < right;
        });
        for (const auto &child : children) append(child);
    };
    append(root);
    return thread;
}

QJsonObject Part::ensureLogicalComment(const QString &annotationId)
{
    const auto existing = effectiveFedoraProjections().value(QStringLiteral("logical_comment\ncomments\n") + annotationId).toObject();
    if (!existing.isEmpty()) return existing.value(QStringLiteral("deleted")).toBool() ? QJsonObject() : existing;
    return {};
}

bool Part::saveUnplacedComment(const QString &annotationId, const QJsonObject &patch)
{
    auto logical = ensureLogicalComment(annotationId);
    if (logical.isEmpty()) return false;
    for (auto it = patch.begin(); it != patch.end(); ++it) logical.insert(it.key(), it.value());
    logical.insert(QStringLiteral("revision"), logical.value(QStringLiteral("revision")).toInteger() + 1);
    logical.insert(QStringLiteral("updated_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    logical.insert(QStringLiteral("origin_id"), Thoughts::Platform::desktopOriginId());
    for (const auto &field : {QStringLiteral("locations"), QStringLiteral("source_file"), QStringLiteral("source_line"), QStringLiteral("source_sha256")}) logical.remove(field);
    return m_fedoraRepository->enqueueLogicalComment(logical);
}

bool Part::openUnplacedComment(const QString &annotationId, int expectedPage)
{
    if (!m_document->isOpened() || !m_pageView) return false;
    const QString id = Thoughts::logicalCommentId(annotationId);
    const QJsonObject record = currentCommentDetailsRecord(id);
    if (record.isEmpty() || record.value(QStringLiteral("deleted")).toBool()) {
        if (auto *panel = qobject_cast<AnnotWindow *>(m_unplacedCommentPanel.data());
            panel && panel->property("commentId").toString() == id) {
            panel->setDetachedThread({});
        }
        return false;
    }

    // Page-only navigation is useful without inventing an annotation rectangle.
    const int page = expectedPage > 0 ? expectedPage : record.value(QStringLiteral("page")).toInt();
    if (page > 0 && page <= int(m_document->pages())) goToPage(uint(page));
    m_pageView->closeReviewAnnotationWindow();
    m_pageView->setReviewSelection(nullptr, -1);
    m_pageView->clearReviewMask();

    // Opening a comment from the list leaves keyboard focus where it was,
    // exactly like the placed-comment path. The editor only takes focus when
    // the user clicks into it, so Backspace there edits text, not the comment.
    const QPointer<QWidget> previousFocusWidget = QApplication::focusWidget();

    auto *panel = qobject_cast<AnnotWindow *>(m_unplacedCommentPanel.data());
    if (panel && panel->property("commentId").toString() == id) {
        // Reuse the window for the same comment instead of rebuilding it, so
        // reopening preserves the thread view and does not re-steal focus.
        panel->setDetachedThread(unplacedCommentThread(id));
        panel->setRelatedInfo(m_pageView->annotationRelatedInfo(id), i18n("No precise location"));
    } else {
        delete m_unplacedCommentPanel.data();
        panel = new AnnotWindow(m_pageView, unplacedCommentThread(id), id, m_document, -1);
        m_unplacedCommentPanel = panel;
        panel->setProperty("commentId", id);
        panel->setRelatedInfo(m_pageView->annotationRelatedInfo(id), i18n("No precise location"));
        connect(panel, &AnnotWindow::threadActionSaveRequested, this, [this, panel, id](const QString &rootId, bool needsAction) {
            const bool success = saveUnplacedComment(rootId, {{QStringLiteral("needs_action"), needsAction}});
            panel->completeThreadActionOperation(success);
            panel->setThreadActionInfo(m_pageView->annotationRelatedInfo(id));
            m_pageView->completeCommentThreadAction(rootId, success);
        });
        connect(panel, &AnnotWindow::detachedContentsSaveRequested, this, [this, panel, id](const QString &contents) {
            if (saveUnplacedComment(id, {{QStringLiteral("contents"), contents}})) {
                panel->annotation()->setContents(contents);
            } else {
                panel->completeRelatedOperation(false, i18n("The comment could not be saved locally."));
            }
        });
        connect(panel, &AnnotWindow::relatedMetadataSaveRequested, this, [this, panel, id](const QVariantMap &patch) {
            if (saveUnplacedComment(id, QJsonObject::fromVariantMap(patch))) {
                panel->setRelatedInfo(m_pageView->annotationRelatedInfo(id), i18n("Saved locally."));
            } else {
                panel->completeRelatedOperation(false, i18n("The details could not be saved locally."));
            }
        });
        connect(panel, &AnnotWindow::relatedChatGptUrlSaveRequested, this, [this, panel, id](const QString &url, bool openAfterSave) {
            if (saveUnplacedComment(id, {{QStringLiteral("chatgpt_url"), url}})) {
                panel->setRelatedInfo(m_pageView->annotationRelatedInfo(id), i18n("Saved locally."));
                if (openAfterSave) Q_EMIT requestOpenCommentChatGpt(url);
            } else {
                panel->completeRelatedOperation(false, i18n("The link could not be saved locally."));
            }
        });
        connect(panel, &AnnotWindow::replyRequested, this, [this, panel, id](const QString &contents) {
            const auto parent = ensureLogicalComment(id);
            auto scope = parent.value(QStringLiteral("scope")).toObject();
            scope.insert(QStringLiteral("reason"), QStringLiteral("inherits_thread_locations"));
            const auto now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
            const KConfigGroup identity(KSharedConfig::openConfig(), QStringLiteral("Identity"));
            const QString replyId = QStringLiteral("thoughts-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
            const QJsonObject reply {
                {QStringLiteral("schema_version"), 1}, {QStringLiteral("comment_id"), replyId},
                {QStringLiteral("thread_root_id"), parent.value(QStringLiteral("thread_root_id"))}, {QStringLiteral("parent_id"), id},
                {QStringLiteral("revision"), 1}, {QStringLiteral("created_at"), now}, {QStringLiteral("updated_at"), now},
                {QStringLiteral("origin_id"), Thoughts::Platform::desktopOriginId()}, {QStringLiteral("deleted"), false},
                {QStringLiteral("contents"), contents.trimmed()}, {QStringLiteral("author"), identity.readEntry(QStringLiteral("IdentityAuthor"), qEnvironmentVariable("USER", qEnvironmentVariable("USERNAME")))},
                {QStringLiteral("subtype"), QStringLiteral("Text")}, {QStringLiteral("priority"), QStringLiteral("normal")},
                {QStringLiteral("selected_text"), QString()}, {QStringLiteral("color"), parent.value(QStringLiteral("color"))},
                {QStringLiteral("location"), QJsonValue(QJsonValue::Null)}, {QStringLiteral("chatgpt_url"), QString()},
                {QStringLiteral("origin"), parent.value(QStringLiteral("origin"))}, {QStringLiteral("scope"), scope}};
            const bool saved = !parent.isEmpty() && !contents.trimmed().isEmpty() && m_fedoraRepository->enqueueLogicalComment(reply);
            if (saved) panel->setDetachedThread(unplacedCommentThread(id));
            else panel->completeRelatedOperation(false, i18n("The reply could not be saved locally."));
            panel->completeReplyOperation(saved);
        });
        connect(panel, &AnnotWindow::relatedOpenCodexRequested, this, [this, id] { Q_EMIT requestOpenCommentCodex(localFilePath(), id); });
        connect(panel, &AnnotWindow::relatedOpenChatGptRequested, this, &Part::requestOpenCommentChatGpt);
        connect(panel, &AnnotWindow::relatedOpenSourceRequested, this, &Part::requestOpenCommentSource);
        connect(panel, &AnnotWindow::relatedOpenOverviewRequested, this, [this, id, page] { Q_EMIT requestCommentsOverviewForComment(localFilePath(), id, page); });
    }
    panel->show();
    panel->raise();
    if (previousFocusWidget) {
        previousFocusWidget->setFocus(Qt::OtherFocusReason);
    }
    Q_EMIT unplacedCommentOpened(id, page > 0 && page <= int(m_document->pages()) ? page : 0);
    return true;
}

bool Part::openReviewAnnotation(const QString &annotationId, int expectedPage)
{
    if (!m_document->isOpened() || !m_reviewsWidget || annotationId.isEmpty()) {
        return false;
    }

    if (shouldSyncDocsComments(url()) && (!m_docsOpenedPdfIdentityVerified || m_docsCommentLoadWatcher->isRunning() || m_docsCommentRefreshQueued)) {
        m_pendingReviewAnnotationId = annotationId;
        m_pendingReviewAnnotationPage = expectedPage;
        return true;
    }
    QString id = annotationId;
    if (id.startsWith(QLatin1String("id:"))) {
        id.remove(0, 3);
    }

    const auto annotationOnPage = [this, &id](int pageNumber) -> Okular::Annotation * {
        if (pageNumber < 0 || pageNumber >= static_cast<int>(m_document->pages())) {
            return nullptr;
        }
        const Okular::Page *page = m_document->page(pageNumber);
        if (Okular::Annotation *annotation = page->annotation(id)) {
            return annotation;
        }
        for (Okular::Annotation *annotation : page->annotations()) {
            if (annotation->uniqueName() == id) {
                return annotation;
            }
        }
        return nullptr;
    };

    const int preferredPage = expectedPage - 1;
    if (Okular::Annotation *annotation = annotationOnPage(preferredPage)) {
        delete m_unplacedCommentPanel.data();
        return m_reviewsWidget->selectAndOpenAnnotation(annotation, preferredPage);
    }
    for (uint pageNumber = 0; pageNumber < m_document->pages(); ++pageNumber) {
        if (static_cast<int>(pageNumber) == preferredPage) {
            continue;
        }
        if (Okular::Annotation *annotation = annotationOnPage(static_cast<int>(pageNumber))) {
            delete m_unplacedCommentPanel.data();
            return m_reviewsWidget->selectAndOpenAnnotation(annotation, static_cast<int>(pageNumber));
        }
    }
    return openUnplacedComment(id, expectedPage);
}

QVariantMap Part::openLogicalCommentLocation(const QString &commentId, int page, const QVariantMap &location)
{
    if (!m_document->isOpened() || !m_pageView || commentId.isEmpty() || page < 1
        || page > static_cast<int>(m_document->pages())) {
        return {{QStringLiteral("opened"), false}, {QStringLiteral("error"), i18n("The logical comment location is not available in this PDF revision.")}};
    }
    const QString status = location.value(QStringLiteral("status")).toString();
    if (status == QLatin1String("page_only")) {
        const bool opened = openUnplacedComment(commentId, page);
        return {{QStringLiteral("opened"), opened}, {QStringLiteral("exactMarker"), false},
            {QStringLiteral("error"), opened ? QString() : i18n("The comment is unavailable.")}};
    }
    if (status != QLatin1String("exact")) {
        return {{QStringLiteral("opened"), false}, {QStringLiteral("error"), i18n("This placement is unavailable. Open its source instead.")}};
    }
    QVariantList rects;
    const QVariantList regions = location.value(QStringLiteral("regions")).toList();
    for (const QVariant &encoded : regions) {
        const QVariantList rect = encoded.toList();
        if (rect.size() == 4) {
            rects.append(QVariant(rect));
        }
    }
    if (rects.isEmpty()) {
        const QVariantList point = location.value(QStringLiteral("point")).toList();
        if (Thoughts::validNormalizedReviewCoordinates(QJsonArray::fromVariantList(point), 2)) {
            const double x = point.at(0).toDouble();
            const double y = point.at(1).toDouble();
            rects.append(QVariant(QVariantList {qBound(0.0, x - 0.008, 0.984),
                                       qBound(0.0, y - 0.008, 0.984),
                                       qBound(0.016, x + 0.008, 1.0),
                                       qBound(0.016, y + 0.008, 1.0)}));
        }
    }
    if (rects.isEmpty() || !m_pageView->showReviewMask(page, rects)) {
        return {{QStringLiteral("opened"), false}, {QStringLiteral("error"), i18n("The annotation’s PDF placement cannot be displayed in this PDF revision.")}};
    }
    return {{QStringLiteral("opened"), true}, {QStringLiteral("exactMarker"), true}};
}

void Part::exportProjectPdf(const QJsonObject &project, const QString &destination)
{
    auto *progress = new QProgressDialog(i18n("Requesting export…"), i18n("Close"), 0, 0, widget());
    progress->setAttribute(Qt::WA_DeleteOnClose);
    progress->setAutoClose(false);
    connect(progress, &QProgressDialog::canceled, progress, &QObject::deleteLater);
    FedoraRepository::exportProjectPdf(m_docsRenderRoot, project, destination, progress,
        [progress](const QString &message) { progress->setLabelText(message); },
        [this, progress, destination](bool older, const QString &error) {
            progress->close();
            if (!error.isEmpty()) KMessageBox::error(widget(), error);
            else KMessageBox::information(widget(), older ? i18n("Export saved to %1. It contains the captured older project revision.", destination) : i18n("Export saved to %1.", destination));
        });
    progress->show();
}

QVariantMap Part::reviewDocumentPayload() const
{
    auto payload = m_pageView ? m_pageView->reviewDocumentPayload() : QVariantMap {{QStringLiteral("error"), i18n("The page view is unavailable.")}};
    const auto identity = Thoughts::DocumentIdentityStore(m_docsRenderRoot).forFile(payload.value(QStringLiteral("pdf_path")).toString(), payload.value(QStringLiteral("pdf_sha256")).toString());
    payload.insert(QStringLiteral("document_id"), identity.value(QStringLiteral("document_id")).toString());
    return payload;
}

QVariantMap Part::reviewDocumentPayloadForPage(int page) const
{
    const auto reading = Thoughts::ProjectReadingContext::load(m_docsOpenedPdfPath);
    if (!reading) return reviewDocumentPayload();
    const auto *child = reading->locate(page - 1);
    if (!child) return {{QStringLiteral("error"), i18n("Select a page inside one document.")}};
    const auto path = QDir(QFileInfo(m_docsOpenedPdfPath).absolutePath()).filePath(child->filename);
    if (Thoughts::artifactFileSha256(path) != child->document.pdfSha256.toLatin1())
        return {{QStringLiteral("error"), i18n("This document is awaiting its complete project update.")}};
    return {{QStringLiteral("pdf_path"), path}, {QStringLiteral("pdf_stem"), child->stem()},
        {QStringLiteral("pdf_sha256"), child->document.pdfSha256}, {QStringLiteral("document_id"), child->document.id},
        {QStringLiteral("page_count"), child->document.pages}, {QStringLiteral("page_offset"), child->document.firstPage}};
}

bool Part::showReviewMask(int page, const QVariantList &rects)
{
    return m_pageView && m_pageView->showReviewMask(page, rects);
}

bool Part::showReviewMasks(const QVariantList &regions)
{
    return showReviewMasks(regions, true);
}

bool Part::showReviewMasks(const QVariantList &regions, bool navigate)
{
    // A card made in an HTML attachment reviews in its page (no PDF place, never in the edition).
    if (Thoughts::isHtmlAttachmentCard(m_reviewCardRecord)) return presentAttachmentReview();
    // A review uses the view the document is open in; the card set by setReviewFeedbackCard.
    if (presentHtmlReview(regions)) return true;
    return m_pageView && m_pageView->showReviewMasks(regions, PageView::ReviewMaskStyle::Hidden, navigate);
}

void Part::setReviewMaskInverted(bool inverted)
{
    if (m_attachmentReviewShown && m_attachmentReader) {
        m_attachmentReader->setReviewInverted(inverted);
        return;
    }
    if (m_htmlReviewShown && m_htmlReader) {
        m_htmlReader->setReviewInverted(inverted);
        return;
    }
    if (m_pageView) {
        m_pageView->setReviewMaskInverted(inverted);
    }
}

void Part::revealReviewMask()
{
    if (m_attachmentReviewShown && m_attachmentReader) {
        m_attachmentReader->reveal();
        return;
    }
    if (m_htmlReviewShown && m_htmlReader) {
        m_htmlReader->reveal();
        refreshHtmlFeedback();
        if (m_pageView && m_pageView->reviewFeedback()->hasCard()) Q_EMIT reviewFeedbackChanged();
        return;
    }
    if (m_pageView) {
        m_pageView->revealReviewMask();
        // The card's answer feedback is shown from now on.
        if (m_pageView->reviewFeedback()->hasCard()) Q_EMIT reviewFeedbackChanged();
    }
}

bool Part::previewReviewCardAreas(const QVariantList &regions)
{
    if (!m_pageView || m_attachmentReviewShown || m_htmlReviewShown || reviewMaskBlockCount() > 0) return false;
    return m_pageView->showReviewMasks(regions, PageView::ReviewMaskStyle::SidebarHighlight, true);
}

void Part::clearReviewCardAreasPreview()
{
    if (m_pageView) m_pageView->clearSidebarReviewHighlight();
}

QVariantList Part::reviewDocumentOutline() const
{
    if (!m_document || !m_document->isOpened() || Thoughts::ProjectReadingContext::load(m_docsOpenedPdfPath)) return {};
    const Okular::DocumentSynopsis *synopsis = m_document->documentSynopsis();
    if (!synopsis) return {};
    std::function<QVariantList(const QDomNode &, int)> entries = [this, &entries](const QDomNode &parent, int depth) {
        QVariantList items;
        if (depth > 64) return items;
        for (QDomNode node = parent.firstChild(); !node.isNull(); node = node.nextSibling()) {
            const QDomElement element = node.toElement();
            if (element.isNull()) continue;
            Okular::DocumentViewport viewport;
            // A destination in another file is no place in this PDF.
            if (element.attribute(QStringLiteral("ExternalFileName")).isEmpty()) {
                if (element.hasAttribute(QStringLiteral("Viewport"))) {
                    viewport = Okular::DocumentViewport(element.attribute(QStringLiteral("Viewport")));
                } else if (element.hasAttribute(QStringLiteral("ViewportName"))) {
                    const QString named = m_document->metaData(QStringLiteral("NamedViewport"), element.attribute(QStringLiteral("ViewportName"))).toString();
                    if (!named.isEmpty()) viewport = Okular::DocumentViewport(named);
                }
            }
            const bool placed = viewport.isValid() && viewport.pageNumber < int(m_document->pages());
            const bool topLeft = viewport.rePos.enabled && viewport.rePos.pos == Okular::DocumentViewport::TopLeft;
            items.append(QVariantMap{{QStringLiteral("title"), element.tagName()},
                                     {QStringLiteral("page"), placed ? viewport.pageNumber + 1 : 0},
                                     {QStringLiteral("top"), placed && topLeft ? std::clamp(viewport.rePos.normalizedY, 0.0, 1.0) : 0.0},
                                     {QStringLiteral("children"), entries(element, depth + 1)}});
        }
        return items;
    };
    return entries(*synopsis, 0);
}

int Part::reviewMaskBlockCount() const
{
    if (m_attachmentReviewShown && m_attachmentReader) return m_attachmentReader->reviewBlockCount();
    if (m_htmlReviewShown && m_htmlReader) return m_htmlReader->reviewBlockCount();
    return m_pageView ? m_pageView->reviewMaskBlockCount() : 0;
}

bool Part::focusReviewMaskBlock(int index)
{
    if (m_attachmentReviewShown && m_attachmentReader) return m_attachmentReader->focusReviewBlock(index);
    if (m_htmlReviewShown && m_htmlReader) return m_htmlReader->focusReviewBlock(index);
    return m_pageView && m_pageView->focusReviewMaskBlock(index);
}

void Part::setReviewFeedbackCard(const QJsonObject &card, const QVariantList &regions)
{
    m_reviewCardRecord = card;
    m_reviewCardRegions = regions;
    if (!m_pageView) return;
    const QString path = localFilePath();
    QString documentId = Thoughts::DocumentIdentityStore(m_docsRenderRoot).forFile(path).value(QStringLiteral("document_id")).toString();
    const QJsonObject primary = card.value(QStringLiteral("primary")).toObject();
    if (documentId.isEmpty()) documentId = primary.value(QStringLiteral("pdf_document_id")).toString(primary.value(QStringLiteral("document_id")).toString());
    QString sha256 = !m_docsOpenedPdfSha256.isEmpty() && QFileInfo(m_docsOpenedPdfPath) == QFileInfo(path) ? m_docsOpenedPdfSha256 : QString();
    if (sha256.isEmpty()) sha256 = Thoughts::FeedbackVersion::cachedFileSha256(path);
    auto *feedback = m_pageView->reviewFeedback();
    feedback->setCard(card, regions, documentId, sha256);
    // Feedback written now records this PDF (built from a source when the Library or the card's markers say so).
    feedback->setReviewVersion(Thoughts::FeedbackVersion::forPdf(m_docsRenderRoot, path,
        !primary.value(QStringLiteral("source_anchor")).toObject().value(QStringLiteral("segments")).toArray().isEmpty(), sha256));
    // Its own feedback and that of the other cards of its document (shown where it lies in this answer).
    feedback->setRecords(Thoughts::ReviewCardFeedback::recordsForCard(m_fedoraProjections, feedback->cardId(), feedback->document(), feedback->homeCards(), true));
}

void Part::refreshDocumentFeedback()
{
    if (!m_pageView) return;
    m_attachmentVersions.clear();
    auto *feedback = m_pageView->documentFeedback();
    const QString path = url().isLocalFile() ? localFilePath() : QString();
    m_projectFeedback = {};
    if (path.isEmpty() || !m_document->isOpened()) {
        feedback->clear();
        return;
    }
    // A Project read as one PDF numbers its pages across its documents: each document's feedback
    // shows at its pages there, and what is given there goes to its own document.
    if (Thoughts::ProjectReadingContext::load(path) || (!m_docsOpenedPdfPath.isEmpty() && Thoughts::ProjectReadingContext::load(m_docsOpenedPdfPath))) {
        if (!refreshProjectDocumentFeedback(Thoughts::ProjectReadingContext::load(path) ? path : m_docsOpenedPdfPath)) {
            m_projectFeedback = {};
            feedback->clear();
        }
        return;
    }
    // The Library identity of these bytes: the entry is returned only when the file is its PDF.
    const QString verified = !m_docsOpenedPdfSha256.isEmpty() && QFileInfo(m_docsOpenedPdfPath) == QFileInfo(path) ? m_docsOpenedPdfSha256 : QString();
    const QJsonObject identity = Thoughts::DocumentIdentityStore(m_docsRenderRoot).forFile(path, verified);
    const QString documentId = identity.value(QStringLiteral("document_id")).toString();
    const QString sha256 = identity.value(QStringLiteral("pdf_sha256")).toString().toLower();
    if (!Thoughts::DocumentIdentityStore::validId(documentId) || sha256.size() != 64) {
        feedback->clear();
        return;
    }
    feedback->setDocument(documentId, sha256);
    // Feedback written now records this PDF version.
    feedback->setReviewVersion(Thoughts::FeedbackVersion::forPdf(m_docsRenderRoot, path, false, sha256));
    feedback->setRecords(Thoughts::DocumentFeedback::recordsOfDocument(m_fedoraProjections, documentId, true));
}

bool Part::refreshProjectDocumentFeedback(const QString &path)
{
    const auto reading = Thoughts::ProjectReadingContext::load(path);
    if (!reading || reading->revision.size() != 64) return false;
    ProjectFeedbackState state;
    state.virtualId = Thoughts::ProjectFeedback::virtualDocumentId(reading->revision);
    state.virtualSha = reading->revision.toLower();
    state.reading = reading->reading;
    QList<QJsonObject> records;
    for (const auto &document : reading->documents) {
        state.filenames.append({document.document.id, document.filename});
        for (const QJsonObject &record : Thoughts::DocumentFeedback::recordsOfDocument(m_fedoraProjections, document.document.id, true)) {
            state.owners.insert(record.value(QStringLiteral("feedback_id")).toString(), document.document.id);
            records.append(Thoughts::ProjectFeedback::toProject(record, document.document, state.virtualId, state.virtualSha));
        }
    }
    m_projectFeedback = state;
    auto *feedback = m_pageView->documentFeedback();
    feedback->setDocument(state.virtualId, state.virtualSha);
    // Stamped with the reading's hash here; projectFeedbackToDocument puts each document's own back.
    feedback->setReviewVersion(Thoughts::FeedbackVersion::make(state.virtualSha, false));
    feedback->setRecords(records);
    return true;
}

std::optional<QJsonObject> Part::projectFeedbackToDocument(const QJsonObject &record, QString *error)
{
    if (!m_projectFeedback.reading || Thoughts::DocumentFeedback::documentIdOf(record).compare(m_projectFeedback.virtualId, Qt::CaseInsensitive) != 0)
        return record;
    const Thoughts::ProjectReading &reading = *m_projectFeedback.reading;
    const QString feedbackId = record.value(QStringLiteral("feedback_id")).toString();
    const Thoughts::ReadingDocument *document = nullptr;
    // Feedback already there stays with its document; new feedback goes to the one its passage is in.
    if (const QString owner = m_projectFeedback.owners.value(feedbackId); !owner.isEmpty()) {
        for (const auto &candidate : reading.documents)
            if (candidate.id == owner) document = &candidate;
    } else {
        document = Thoughts::ProjectFeedback::documentOf(record, reading);
    }
    if (!document) {
        if (error) *error = i18n("Select a passage within one document of the Project.");
        return std::nullopt;
    }
    QJsonObject own = Thoughts::ProjectFeedback::fromProject(record, *document, m_projectFeedback.virtualSha);
    // Stamped here (with the reading's hash, now the document's): the document's own PDF version,
    // as if the feedback had been given in it. A stamp of an earlier version stays as it was.
    if (own.value(Thoughts::FeedbackVersion::field()).toObject().value(QStringLiteral("pdf_sha256")).toString().compare(document->pdfSha256, Qt::CaseInsensitive) == 0) {
        QString filename;
        for (const auto &[id, name] : std::as_const(m_projectFeedback.filenames))
            if (id == document->id) filename = name;
        const QJsonObject version = Thoughts::FeedbackVersion::make(document->pdfSha256.toLower(),
                                                                    Thoughts::FeedbackVersion::librarySourceBuilt(m_docsRenderRoot, filename));
        own = Thoughts::FeedbackVersion::stamped(own, version);
    }
    m_projectFeedback.owners.insert(feedbackId, document->id);
    return own;
}

void Part::showDocumentFeedbackList()
{
    if (!m_pageView) return;
    const auto *feedback = m_pageView->documentFeedback();
    const QList<QJsonObject> shown = feedback->shownRecords();
    if (m_reviewsWidget) {
        QSet<QString> earlier;
        for (const QJsonObject &record : shown) {
            if (!feedback->isCurrent(record)) earlier.insert(record.value(QStringLiteral("feedback_id")).toString());
        }
        m_reviewsWidget->setEarlierFeedback(earlier, feedback->earlierCount(), feedback->showsEarlier());
        m_reviewsWidget->setDocumentFeedback(shown);
    }
    if (m_attachmentReader) m_attachmentReader->setFeedback(shown);
}

bool Part::showAnnotationsForNewEntry()
{
    if (m_sidebar->isTransientWidgetVisible() || m_pageView->reviewingCard()) return false;
    chooseSidebarDisplay(m_reviewsWidget);
    return true;
}

void Part::setReviewFeedbackHomeCards(const QJsonObject &cards)
{
    if (!m_pageView) return;
    Thoughts::ReviewCardFeedback::HomeCards homeCards;
    for (auto it = cards.constBegin(); it != cards.constEnd(); ++it) homeCards.insert(it.key(), it.value().toObject());
    m_pageView->reviewFeedback()->setHomeCards(homeCards);
}

QVariantList Part::reviewFeedbackItems()
{
    const bool htmlAnswerShown = m_htmlReviewShown && m_htmlReader && m_htmlReader->reviewRevealed() && m_pageView && m_pageView->reviewFeedback()->hasCard();
    if (!m_pageView || (!m_pageView->reviewFeedbackVisible() && !htmlAnswerShown)) return {};
    QVariantList items = m_pageView->reviewFeedback()->itemMaps();
    // The file shown, for pictures of passages given on it.
    for (QVariant &item : items) {
        QVariantMap map = item.toMap();
        map.insert(QStringLiteral("displayed_pdf_path"), localFilePath());
        item = map;
    }
    return items;
}

bool Part::setHistorySidebarPanel(QWidget *panel)
{
    if (!m_sidebar || !panel) return false;
    if (m_historyPanel != panel) {
        if (m_historyPanel) m_sidebar->removeItem(m_historyPanel);
        m_historyPanel = panel;
        m_sidebar->addItem(panel, QIcon::fromTheme(QStringLiteral("view-history")), i18n("History"));
    }
    m_sidebarHistoryAction->setVisible(true);
    updateSidebarDisplayActions();
    return true;
}

bool Part::showHistorySidebar()
{
    if (!m_historyPanel) return false;
    chooseSidebarDisplay(m_historyPanel);
    return m_sidebar->currentItem() == m_historyPanel && m_showLeftPanel->isChecked();
}

bool Part::setAgentNotesSidebarPanel(QWidget *panel)
{
    if (!m_sidebar || !panel) return false;
    if (m_agentNotesPanel != panel) {
        if (m_agentNotesPanel) {
            disconnect(m_agentNotesPanel, &QWidget::windowTitleChanged, m_sidebarAgentNotesAction, nullptr);
            m_sidebar->removeItem(m_agentNotesPanel);
        }
        m_agentNotesPanel = panel;
        m_sidebar->addItem(panel, QIcon::fromTheme(QStringLiteral("mail-message"), QIcon::fromTheme(QStringLiteral("dialog-messages"))), i18n("Agent Notes"));
        // "Agent Notes (2 unread)", as the panel counts them.
        const auto name = [this](const QString &title) { m_sidebarAgentNotesAction->setText(title.isEmpty() ? i18n("Agent Notes") : title); };
        name(panel->windowTitle());
        connect(panel, &QWidget::windowTitleChanged, m_sidebarAgentNotesAction, name);
    }
    m_sidebarAgentNotesAction->setVisible(true);
    updateSidebarDisplayActions();
    return true;
}

bool Part::showAgentNotesSidebar()
{
    if (!m_agentNotesPanel) return false;
    chooseSidebarDisplay(m_agentNotesPanel);
    return m_sidebar->currentItem() == m_agentNotesPanel && m_showLeftPanel->isChecked();
}


QVariantMap Part::reviewFeedbackStates()
{
    return m_pageView && m_pageView->reviewFeedback()->hasCard() ? m_pageView->reviewFeedback()->states() : QVariantMap();
}

bool Part::showPassageRegions(const QVariantList &regions)
{
    if (!m_pageView || !m_document->isOpened()) return false;
    const auto passage = Thoughts::ReviewCardFeedback::regionsFromVariant(regions);
    if (passage.isEmpty()) return false;
    m_pageView->showPassageRegions(passage);
    return true;
}

bool Part::openReviewFeedback(const QString &feedbackId)
{
    if (m_htmlReviewShown && m_htmlReader && m_htmlReader->reviewRevealed()) {
        if (!m_pageView || m_pageView->reviewFeedback()->record(feedbackId).isEmpty()) return false;
        htmlItemTapped(QJsonObject{{QStringLiteral("kind"), QStringLiteral("feedback")}, {QStringLiteral("id"), feedbackId}});
        return true;
    }
    return m_pageView && m_pageView->reviewFeedbackVisible() && m_pageView->openReviewFeedbackEditor(feedbackId);
}

void Part::clearReviewMask()
{
    m_htmlReviewShown = false;
    m_reviewCardRecord = {};
    if (m_attachmentReader && (m_attachmentReviewShown || !m_attachmentReader->reviewCardId().isEmpty())) {
        m_attachmentReviewShown = false;
        m_attachmentReader->clearReview();
        m_attachmentView->setNotice(QString(), QString());
        updateAttachmentSelectionAction();
        // A page opened for the review goes when the review moves on to a card elsewhere (the next
        // card is presented right after this clear: the same page stays without a flicker).
        if (m_attachmentForReview) {
            QTimer::singleShot(0, this, [this] {
                if (m_attachmentForReview && !m_attachmentReviewShown) hideHtmlAttachment();
            });
        }
    }
    if (m_htmlReader) {
        m_htmlReader->clearReview();
    }
    if (m_pageView) {
        m_pageView->clearReviewMask();
    }
}

namespace
{

QPair<int, Okular::Annotation *> findReviewAnnotation(Okular::Document *document, const QString &annotationId, int expectedPage)
{
    if (!document || !document->isOpened() || annotationId.isEmpty()) {
        return {-1, nullptr};
    }

    QString id = annotationId;
    if (id.startsWith(QLatin1String("id:"))) {
        id.remove(0, 3);
    }

    const auto annotationOnPage = [document, &id](int pageNumber) -> Okular::Annotation * {
        if (pageNumber < 0 || pageNumber >= static_cast<int>(document->pages())) {
            return nullptr;
        }
        const Okular::Page *page = document->page(pageNumber);
        if (Okular::Annotation *annotation = page->annotation(id)) {
            return annotation;
        }
        for (Okular::Annotation *annotation : page->annotations()) {
            if (annotation->uniqueName() == id) {
                return annotation;
            }
        }
        return nullptr;
    };

    const int preferredPage = expectedPage - 1;
    if (Okular::Annotation *annotation = annotationOnPage(preferredPage)) {
        return {preferredPage, annotation};
    }
    for (uint pageNumber = 0; pageNumber < document->pages(); ++pageNumber) {
        if (static_cast<int>(pageNumber) == preferredPage) {
            continue;
        }
        if (Okular::Annotation *annotation = annotationOnPage(static_cast<int>(pageNumber))) {
            return {static_cast<int>(pageNumber), annotation};
        }
    }
    return {-1, nullptr};
}

QVariantMap
reviewChangeResult(Okular::Document *document, const QList<int> &reviewUndoIndices, const QList<int> &reviewRedoIndices, bool success, const QString &error = QString(), bool partial = false, bool changed = false, bool historyReset = false)
{
    return {
        {QStringLiteral("success"), success},
        {QStringLiteral("partial"), partial},
        {QStringLiteral("changed"), changed},
        {QStringLiteral("historyReset"), historyReset},
        {QStringLiteral("error"), error},
        {QStringLiteral("canUndo"), document && !reviewUndoIndices.isEmpty()},
        {QStringLiteral("canRedo"), document && !reviewRedoIndices.isEmpty()},
    };
}

bool validateReviewMetadataPatch(const QJsonObject &patch, QString *error)
{
    for (auto iterator = patch.begin(); iterator != patch.end(); ++iterator) {
        if (iterator.key() != QLatin1String("priority") && iterator.key() != QLatin1String("location")) {
            *error = i18n("The Overview edit contains unsupported comment metadata.");
            return false;
        }
    }
    if (patch.contains(QStringLiteral("priority"))) {
        const QJsonValue priority = patch.value(QStringLiteral("priority"));
        if (!priority.isString() || (priority.toString() != QLatin1String("normal") && priority.toString() != QLatin1String("high"))) {
            *error = i18n("The comment priority must be Normal or High.");
            return false;
        }
    }
    if (patch.contains(QStringLiteral("location")) && !patch.value(QStringLiteral("location")).isNull()) {
        const QJsonObject location = patch.value(QStringLiteral("location")).toObject();
        const double latitude = location.value(QStringLiteral("latitude")).toDouble(std::numeric_limits<double>::quiet_NaN());
        const double longitude = location.value(QStringLiteral("longitude")).toDouble(std::numeric_limits<double>::quiet_NaN());
        if (location.value(QStringLiteral("label")).toString().trimmed().isEmpty() || !std::isfinite(latitude) || latitude < -90.0 || latitude > 90.0 || !std::isfinite(longitude) || longitude < -180.0 || longitude > 180.0) {
            *error = i18n("The comment location must have a place name and valid latitude and longitude.");
            return false;
        }
    }
    return true;
}

}

QVariantMap Part::addReviewAnnotationReply(const QString &parentAnnotationId, int expectedPage, const QString &contents)
{
    const QString replyContents = contents.trimmed();
    if (replyContents.isEmpty()) {
        return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, i18n("A reply cannot be empty."));
    }
    const auto [pageNumber, parentAnnotation] = findReviewAnnotation(m_document, parentAnnotationId, expectedPage);
    if (!parentAnnotation) {
        return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, i18n("The comment could not be found."));
    }
    if (!m_document->isAllowed(Okular::AllowNotes)) {
        return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, i18n("Replies are not allowed for this document."));
    }

    const QString stableParentId = Thoughts::logicalCommentId(parentAnnotation->uniqueName());
    if (stableParentId.isEmpty()) {
        return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, i18n("The comment has no stable identifier."));
    }

    auto *reply = new Okular::TextAnnotation();
    reply->setTextType(Okular::TextAnnotation::Linked);
    reply->setTextIcon(QStringLiteral("Comment"));
    reply->setBoundingRectangle(parentAnnotation->boundingRectangle());
    reply->setContents(replyContents);
    reply->setAuthor(Okular::Settings::identityAuthor());
    const QDateTime now = QDateTime::currentDateTime();
    reply->setCreationDate(now);
    reply->setModificationDate(now);
    reply->style() = parentAnnotation->style();
    reply->setFlags((parentAnnotation->flags()
                     & ~(Okular::Annotation::DenyWrite | Okular::Annotation::DenyDelete | Okular::Annotation::External | Okular::Annotation::ExternallyDrawn
                         | Okular::Annotation::BeingMoved | Okular::Annotation::BeingResized))
                    | Okular::Annotation::Hidden);
    reply->window().setSummary(i18n("Reply"));
    const QString replyId = QStringLiteral("thoughts-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    reply->setUniqueName(replyId);
    reply->setCreatorApplication(QStringLiteral("thoughts"));
    reply->setPreviousAnnotation(parentAnnotation);

    const bool documentWasClean = !isModified();
    {
        QScopedValueRollback<bool> suppressCapture(m_suppressDocsCommentMutationCapture, shouldSyncDocsComments(url()));
        m_document->addPageAnnotation(pageNumber, reply);
    }
    if (shouldSyncDocsComments(url())) {
        // Capture is deliberately suppressed above; still register the native
        // object so the first validated refresh cannot add a duplicate reply.
        m_docsProjectedAnnotations.insert(replyId, qMakePair(pageNumber, reply));
        m_docsNewCommentIds.insert(replyId);
    }

    QString error;
    bool saved = false;
    if (shouldSyncDocsComments(url())) {
        saved = true;
        if (currentDocsCommentRecord(stableParentId).isEmpty()) {
            saved = queueDocsCommentUpsert(stableParentId, {}, false, &error);
            if (saved) {
                m_pendingDocsCommentMutationIds.remove(stableParentId);
            }
        }
        if (saved) {
            saved = queueDocsCommentUpsert(replyId, QJsonObject {{QStringLiteral("in_reply_to"), stableParentId}}, documentWasClean, &error);
        }
    } else {
        saved = saveFile();
    }
    if (!saved || (!shouldSyncDocsComments(url()) && isModified())) {
        QScopedValueRollback<bool> suppressCapture(m_suppressDocsCommentMutationCapture, true);
        m_document->undo();
        const QString reason = error.isEmpty() ? i18n("The reply could not be saved.") : i18n("The reply could not be autosaved: %1", error);
        return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, reason);
    }

    const bool historyReset = !m_reviewUndoIndices.isEmpty() || !m_reviewRedoIndices.isEmpty();
    m_reviewUndoIndices.clear();
    m_reviewRedoIndices.clear();
    m_reviewMetadataHistory.clear();
    m_reviewSidecarDeletionHistory.clear();
    QVariantMap result = reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, true, QString(), false, true, historyReset);
    result.insert(QStringLiteral("replyId"), replyId);
    result.insert(QStringLiteral("page"), pageNumber + 1);
    return result;
}

QVariantMap Part::editReviewAnnotation(const QString &annotationId, int expectedPage, const QString &contents, const QVariantMap &metadataPatch)
{
    const QJsonObject jsonMetadataPatch = QJsonObject::fromVariantMap(metadataPatch);
    QString metadataError;
    if (!validateReviewMetadataPatch(jsonMetadataPatch, &metadataError)) {
        return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, metadataError);
    }
    const auto [pageNumber, annotation] = findReviewAnnotation(m_document, annotationId, expectedPage);
    if (!annotation) {
        return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, i18n("The comment could not be found."));
    }

    const Thoughts::CommentFragmentGroup group = Thoughts::commentFragmentGroup(m_document, annotation, pageNumber);
    QList<Okular::Document::PageAnnotation> fragments;
    for (const Thoughts::CommentFragment &fragment : group.fragments) {
        if (!fragment.annotation || !m_document->canModifyPageAnnotation(fragment.annotation)) {
            return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, i18n("This comment cannot be modified."));
        }
        fragments.append({fragment.page, fragment.annotation});
    }
    if (fragments.isEmpty()) {
        return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, i18n("The comment has no editable annotation."));
    }

    const bool unchanged = jsonMetadataPatch.isEmpty() && fragments.constFirst().second->contents() == contents &&
        std::all_of(fragments.cbegin() + 1, fragments.cend(), [](const Okular::Document::PageAnnotation &fragment) { return fragment.second->contents().isEmpty(); });
    if (unchanged) {
        return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, true);
    }

    bool historyReset = false;
    const int lastReviewUndoToken = m_reviewUndoIndices.isEmpty() ? 0 : m_reviewUndoIndices.constLast();
    const int expectedDocumentUndoIndex = lastReviewUndoToken < 0 && m_reviewSidecarDeletionHistory.contains(lastReviewUndoToken) ? m_reviewSidecarDeletionHistory.value(lastReviewUndoToken).documentUndoIndex : lastReviewUndoToken;
    if (!m_reviewUndoIndices.isEmpty() && expectedDocumentUndoIndex != m_document->undoHistoryIndex()) {
        m_reviewUndoIndices.clear();
        m_reviewRedoIndices.clear();
        m_reviewMetadataHistory.clear();
        m_reviewSidecarDeletionHistory.clear();
        historyReset = true;
    }
    for (int redoIndex : std::as_const(m_reviewRedoIndices)) {
        m_reviewMetadataHistory.remove(redoIndex);
        m_reviewSidecarDeletionHistory.remove(redoIndex);
    }
    m_reviewRedoIndices.clear();
    const bool documentWasClean = !isModified();
    const QJsonObject previousRecord = currentDocsCommentRecord(annotationId);
    const QString previousContents = fragments.constFirst().second->contents();
    QJsonObject previousMetadata;
    QJsonObject currentMetadata;
    for (const QString &field : {QStringLiteral("priority"), QStringLiteral("location")}) {
        if (previousRecord.contains(field)) {
            previousMetadata.insert(field, previousRecord.value(field));
            currentMetadata.insert(field, previousRecord.value(field));
        } else {
            previousMetadata.insert(field, QJsonValue(QJsonValue::Null));
        }
    }
    for (auto iterator = jsonMetadataPatch.begin(); iterator != jsonMetadataPatch.end(); ++iterator) {
        currentMetadata.insert(iterator.key(), iterator.value());
    }
    {
        QScopedValueRollback<bool> suppressCapture(m_suppressDocsCommentMutationCapture, shouldSyncDocsComments(url()));
        m_document->editPageAnnotationGroupContents(fragments, contents, QDateTime::currentDateTimeUtc());
    }
    if (!shouldSyncDocsComments(url())) {
        if (!saveFile() || isModified()) {
            m_document->undo();
            return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, i18n("The comment could not be saved."), false, false, historyReset);
        }
        m_reviewUndoIndices.append(m_document->undoHistoryIndex());
        return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, true, QString(), false, true, historyReset);
    }
    const QString pdfStem = QFileInfo(localFilePath()).completeBaseName();
    QString mutationError;
    if (!queueDocsCommentUpsert(annotationId, jsonMetadataPatch, documentWasClean, &mutationError)) {
        QScopedValueRollback<bool> suppressCapture(m_suppressDocsCommentMutationCapture, true);
        m_document->undo();
        return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, i18n("The comment could not be autosaved: %1", mutationError), false, false, historyReset);
    }
    const int undoIndex = m_document->undoHistoryIndex();
    m_reviewMetadataHistory.insert(undoIndex,
                                   {
                                       annotationId,
                                       pdfStem,
                                       previousContents,
                                       contents,
                                       previousMetadata,
                                       currentMetadata,
                                   });
    m_reviewUndoIndices.append(undoIndex);
    return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, true, QString(), false, true, historyReset);
}

QVariantMap Part::deleteReviewAnnotation(const QString &annotationId, int expectedPage)
{
    return deleteReviewAnnotations({QVariantMap {
        {QStringLiteral("annotationId"), annotationId},
        {QStringLiteral("page"), expectedPage},
    }});
}

QVariantMap Part::updateReviewAnnotationChatGptUrl(const QString &annotationId, const QString &chatGptUrl)
{
    QString validationError;
    const QString normalized = DocsCommentSynchronizer::normalizedChatGptUrl(chatGptUrl, &validationError);
    if (!validationError.isEmpty()) {
        return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, validationError);
    }
    const auto [pageNumber, annotation] = findReviewAnnotation(m_document, annotationId, -1);
    Q_UNUSED(pageNumber)
    if (!annotation) {
        return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, i18n("The comment could not be found."));
    }
    QString error;
    const bool success = queueDocsCommentUpsert(annotationId, QJsonObject {{QStringLiteral("chatgpt_url"), normalized.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(normalized)}}, false, &error);
    return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, success, error, false, success);
}

QVariantMap Part::deleteLegacyReviewAnnotations(const QVariantList &targets)
{
    if (targets.isEmpty()) {
        return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, i18n("No comments were selected for deletion."));
    }

    QList<Okular::Document::PageAnnotation> fragments;
    QSet<Okular::Annotation *> seenFragments;
    QStringList deletedIds;
    QStringList sidecarOnlyIds;
    QSet<QString> seenIds;
    for (const QVariant &targetValue : targets) {
        const QVariantMap target = targetValue.toMap();
        QString annotationId = target.value(QStringLiteral("annotationId")).toString().trimmed();
        const int expectedPage = target.value(QStringLiteral("page")).toInt();
        if (annotationId.startsWith(QLatin1String("id:"))) {
            annotationId.remove(0, 3);
        }
        if (annotationId.isEmpty() || seenIds.contains(annotationId)) {
            continue;
        }

        const auto [pageNumber, annotation] = findReviewAnnotation(m_document, annotationId, expectedPage);
        if (!annotation) {
            if (!shouldSyncDocsComments(url())) {
                return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, i18n("The comment %1 could not be found.", annotationId));
            }
            sidecarOnlyIds.append(annotationId);
            seenIds.insert(annotationId);
            deletedIds.append(annotationId);
            continue;
        }

        const Thoughts::CommentFragmentGroup group = Thoughts::commentFragmentGroup(m_document, annotation, pageNumber);
        if (group.fragments.isEmpty()) {
            return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, i18n("The comment %1 has no removable annotation.", annotationId));
        }
        for (const Thoughts::CommentFragment &fragment : group.fragments) {
            if (!fragment.annotation || !m_document->canRemovePageAnnotation(fragment.annotation)) {
                return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, i18n("The comment %1 cannot be deleted.", annotationId));
            }
            if (!seenFragments.contains(fragment.annotation)) {
                seenFragments.insert(fragment.annotation);
                fragments.append({fragment.page, fragment.annotation});
            }
        }
        seenIds.insert(annotationId);
        deletedIds.append(annotationId);
    }
    if (fragments.isEmpty() && sidecarOnlyIds.isEmpty()) {
        return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, i18n("The selected comments have no removable annotations."));
    }

    bool historyReset = false;
    const int lastReviewUndoToken = m_reviewUndoIndices.isEmpty() ? 0 : m_reviewUndoIndices.constLast();
    const int expectedDocumentUndoIndex = lastReviewUndoToken < 0 && m_reviewSidecarDeletionHistory.contains(lastReviewUndoToken) ? m_reviewSidecarDeletionHistory.value(lastReviewUndoToken).documentUndoIndex : lastReviewUndoToken;
    if (!m_reviewUndoIndices.isEmpty() && expectedDocumentUndoIndex != m_document->undoHistoryIndex()) {
        m_reviewUndoIndices.clear();
        m_reviewRedoIndices.clear();
        m_reviewMetadataHistory.clear();
        m_reviewSidecarDeletionHistory.clear();
        historyReset = true;
    }
    for (int redoIndex : std::as_const(m_reviewRedoIndices)) {
        m_reviewMetadataHistory.remove(redoIndex);
        m_reviewSidecarDeletionHistory.remove(redoIndex);
    }
    m_reviewRedoIndices.clear();

    const QString pdfStem = QFileInfo(localFilePath()).completeBaseName();
    const bool documentWasClean = !isModified();
    Okular::DocsCommentSynchronizer::CommentDeletionResult sidecarDeletion;
    if (shouldSyncDocsComments(url())) {
        for (const QString &id : std::as_const(deletedIds)) {
            const QJsonObject previous = currentDocsCommentRecord(id);
            if (!previous.isEmpty()) {
                sidecarDeletion.previousRecords.append(previous);
            }
        }
        sidecarDeletion.success = true;
    }

    if (!fragments.isEmpty()) {
        {
            QScopedValueRollback<bool> suppressCapture(m_suppressDocsCommentMutationCapture, shouldSyncDocsComments(url()));
            m_document->removePageAnnotations(fragments);
        }
        if (!shouldSyncDocsComments(url())) {
            if (!saveFile()) {
                m_document->undo();
                QVariantMap result = reviewChangeResult(m_document,
                                                        m_reviewUndoIndices,
                                                        m_reviewRedoIndices,
                                                        false,
                                                        i18n("The comments were deleted in memory, but the PDF could not be saved. The deletion was reverted."),
                                                        false,
                                                        false,
                                                        historyReset);
                result.insert(QStringLiteral("deletedIds"), QStringList());
                return result;
            }
        }
    }

    if (shouldSyncDocsComments(url())) {
        QString mutationError;
        if (!queueDocsCommentDeletions(deletedIds, documentWasClean, &mutationError)) {
            if (!fragments.isEmpty()) {
                QScopedValueRollback<bool> suppressCapture(m_suppressDocsCommentMutationCapture, true);
                m_document->undo();
            }
            QVariantMap result = reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, i18n("The comments could not be journaled for deletion: %1", mutationError), false, false, historyReset);
            result.insert(QStringLiteral("deletedIds"), QStringList());
            return result;
        }
        sidecarDeletion.changed = true;
        sidecarDeletion.deletedIds = deletedIds;
    }

    int historyToken = m_document->undoHistoryIndex();
    if (fragments.isEmpty()) {
        historyToken = m_nextReviewSidecarHistoryToken--;
    }
    if (sidecarDeletion.changed) {
        m_reviewSidecarDeletionHistory.insert(historyToken,
                                              {
                                                  pdfStem,
                                                  sidecarDeletion.previousRecords,
                                                  m_document->undoHistoryIndex(),
                                              });
    }
    m_reviewUndoIndices.append(historyToken);
    QVariantMap result;
    if (!shouldSyncDocsComments(url()) && !fragments.isEmpty() && !m_lastDocsCommentSyncSucceeded) {
        result = reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, i18n("The PDF was saved, but the Overview index could not be synchronized: %1", m_lastDocsCommentSyncError), true, true, historyReset);
    } else {
        result = reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, true, QString(), false, true, historyReset);
    }
    result.insert(QStringLiteral("deletedIds"), deletedIds);
    return result;
}

QVariantMap Part::undoReviewChange()
{
    if (!m_applyingCommentStorage && !m_commentUndoTokens.isEmpty()
        && m_commentHistoryDocumentIndex.value(m_commentUndoTokens.last()) == m_document->undoHistoryIndex())
        return undoCommentDeletion(m_commentUndoTokens.last());
    if (m_reviewUndoIndices.isEmpty()) {
        return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, i18n("There is no Overview change to undo."), false, false, true);
    }
    const int undoIndex = m_reviewUndoIndices.constLast();
    const auto sidecarDeletionEntry = m_reviewSidecarDeletionHistory.constFind(undoIndex);
    const int expectedDocumentUndoIndex = sidecarDeletionEntry != m_reviewSidecarDeletionHistory.cend() && undoIndex < 0 ? sidecarDeletionEntry->documentUndoIndex : undoIndex;
    if (expectedDocumentUndoIndex != m_document->undoHistoryIndex()) {
        m_reviewUndoIndices.clear();
        m_reviewRedoIndices.clear();
        m_reviewMetadataHistory.clear();
        m_reviewSidecarDeletionHistory.clear();
        return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, i18n("A newer PDF edit changed the undo history, so this Overview change can no longer be undone here."), false, false, true);
    }
    const auto metadataEntry = m_reviewMetadataHistory.constFind(undoIndex);
    const bool documentWasClean = !isModified();
    const bool hasDocumentChange = undoIndex >= 0;
    const bool sidecarFirst = shouldSyncDocsComments(url());
    if (hasDocumentChange) {
        QScopedValueRollback<bool> suppressCapture(m_suppressDocsCommentMutationCapture, sidecarFirst);
        m_document->undo();
    }
    if (metadataEntry != m_reviewMetadataHistory.cend()) {
        QString mutationError;
        const bool saved = sidecarFirst ? queueDocsCommentUpsert(metadataEntry->annotationId, metadataEntry->previousMetadata, documentWasClean, &mutationError) : saveFile();
        if (!saved) {
            if (hasDocumentChange) {
                QScopedValueRollback<bool> suppressCapture(m_suppressDocsCommentMutationCapture, sidecarFirst);
                m_document->redo();
            }
            return reviewChangeResult(m_document,
                                      m_reviewUndoIndices,
                                      m_reviewRedoIndices,
                                      false,
                                      sidecarFirst ? i18n("The comment could not be restored while undoing: %1", mutationError) : i18n("The undone change could not be saved and was reapplied."));
        }
    } else if (hasDocumentChange && !sidecarFirst) {
        if (!saveFile()) {
            m_document->redo();
            return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, i18n("The undone change could not be saved and was reapplied."));
        }
    }
    if (sidecarDeletionEntry != m_reviewSidecarDeletionHistory.cend()) {
        QString mutationError;
        const bool restored = sidecarFirst ? queueDocsCommentRestores(sidecarDeletionEntry->previousRecords, documentWasClean, &mutationError)
                                           : DocsCommentSynchronizer::restoreSidecarComments(m_docsRenderRoot, sidecarDeletionEntry->pdfStem, sidecarDeletionEntry->previousRecords).success;
        if (!restored) {
            if (hasDocumentChange) {
                QScopedValueRollback<bool> suppressCapture(m_suppressDocsCommentMutationCapture, sidecarFirst);
                m_document->redo();
                if (!sidecarFirst) {
                    saveFile();
                }
            }
            return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, i18n("The sidecar-only comments could not be restored while undoing: %1", mutationError));
        }
    }
    m_reviewRedoIndices.append(m_reviewUndoIndices.takeLast());
    if (!sidecarFirst && hasDocumentChange && metadataEntry == m_reviewMetadataHistory.cend() && !m_lastDocsCommentSyncSucceeded) {
        return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, i18n("The PDF was saved, but the Overview index could not be synchronized: %1", m_lastDocsCommentSyncError), true, true);
    }
    return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, true, QString(), false, true);
}

QVariantMap Part::redoReviewChange()
{
    if (!m_applyingCommentStorage && !m_commentRedoTokens.isEmpty()
        && m_commentHistoryDocumentIndex.value(m_commentRedoTokens.last()) == m_document->undoHistoryIndex())
        return redoCommentDeletion(m_commentRedoTokens.last());
    if (m_reviewRedoIndices.isEmpty()) {
        return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, i18n("There is no Overview change to redo."), false, false, true);
    }
    const int redoIndex = m_reviewRedoIndices.constLast();
    auto sidecarDeletionEntry = m_reviewSidecarDeletionHistory.find(redoIndex);
    const int expectedDocumentUndoIndex = sidecarDeletionEntry != m_reviewSidecarDeletionHistory.end() && redoIndex < 0 ? sidecarDeletionEntry->documentUndoIndex : redoIndex - 1;
    if (expectedDocumentUndoIndex != m_document->undoHistoryIndex()) {
        m_reviewUndoIndices.clear();
        m_reviewRedoIndices.clear();
        m_reviewMetadataHistory.clear();
        m_reviewSidecarDeletionHistory.clear();
        return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, i18n("A newer PDF edit changed the redo history, so this Overview change can no longer be redone here."), false, false, true);
    }
    const auto metadataEntry = m_reviewMetadataHistory.constFind(redoIndex);
    const bool documentWasClean = !isModified();
    const bool hasDocumentChange = redoIndex >= 0;
    const bool sidecarFirst = shouldSyncDocsComments(url());
    Okular::DocsCommentSynchronizer::CommentDeletionResult redoneSidecarDeletion;
    if (sidecarDeletionEntry != m_reviewSidecarDeletionHistory.end()) {
        QStringList sidecarIds;
        for (const QJsonObject &record : std::as_const(sidecarDeletionEntry->previousRecords)) {
            const QString id = record.value(QStringLiteral("id")).toString();
            if (!id.isEmpty()) {
                sidecarIds.append(id);
            }
        }
        if (sidecarFirst) {
            redoneSidecarDeletion.success = true;
            redoneSidecarDeletion.changed = true;
            redoneSidecarDeletion.previousRecords = sidecarDeletionEntry->previousRecords;
        } else {
            redoneSidecarDeletion = Okular::DocsCommentSynchronizer::deleteSidecarComments(m_docsRenderRoot, sidecarDeletionEntry->pdfStem, sidecarIds);
        }
        if (!redoneSidecarDeletion.success) {
            return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, i18n("The comments could not be journaled for deletion while redoing: %1", redoneSidecarDeletion.failureReason));
        }
    }
    if (hasDocumentChange) {
        QScopedValueRollback<bool> suppressCapture(m_suppressDocsCommentMutationCapture, sidecarFirst);
        m_document->redo();
    }
    if (sidecarFirst && sidecarDeletionEntry != m_reviewSidecarDeletionHistory.end()) {
        QStringList sidecarIds;
        for (const QJsonObject &record : std::as_const(sidecarDeletionEntry->previousRecords)) {
            const QString id = record.value(QStringLiteral("id")).toString();
            if (!id.isEmpty()) {
                sidecarIds.append(id);
            }
        }
        QString mutationError;
        if (!queueDocsCommentDeletions(sidecarIds, documentWasClean, &mutationError)) {
            if (hasDocumentChange) {
                QScopedValueRollback<bool> suppressCapture(m_suppressDocsCommentMutationCapture, true);
                m_document->undo();
            }
            return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, i18n("The comments could not be journaled for deletion while redoing: %1", mutationError));
        }
    }
    if (metadataEntry != m_reviewMetadataHistory.cend()) {
        QString mutationError;
        const bool saved = sidecarFirst ? queueDocsCommentUpsert(metadataEntry->annotationId, metadataEntry->currentMetadata, documentWasClean, &mutationError) : saveFile();
        if (!saved) {
            if (hasDocumentChange) {
                QScopedValueRollback<bool> suppressCapture(m_suppressDocsCommentMutationCapture, sidecarFirst);
                m_document->undo();
            }
            if (!sidecarFirst && redoneSidecarDeletion.changed) {
                Okular::DocsCommentSynchronizer::restoreSidecarComments(m_docsRenderRoot, sidecarDeletionEntry->pdfStem, redoneSidecarDeletion.previousRecords);
            }
            return reviewChangeResult(m_document,
                                      m_reviewUndoIndices,
                                      m_reviewRedoIndices,
                                      false,
                                      sidecarFirst ? i18n("The comment could not be restored while redoing: %1", mutationError) : i18n("The redone change could not be saved and was undone again."));
        }
    } else if (hasDocumentChange && !sidecarFirst) {
        if (!saveFile()) {
            m_document->undo();
            if (redoneSidecarDeletion.changed) {
                Okular::DocsCommentSynchronizer::restoreSidecarComments(m_docsRenderRoot, sidecarDeletionEntry->pdfStem, redoneSidecarDeletion.previousRecords);
            }
            return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, i18n("The redone change could not be saved and was undone again."));
        }
    }
    if (redoneSidecarDeletion.changed) {
        sidecarDeletionEntry->previousRecords = redoneSidecarDeletion.previousRecords;
    }
    m_reviewUndoIndices.append(m_reviewRedoIndices.takeLast());
    if (!sidecarFirst && hasDocumentChange && metadataEntry == m_reviewMetadataHistory.cend() && !m_lastDocsCommentSyncSucceeded) {
        return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, false, i18n("The PDF was saved, but the Overview index could not be synchronized: %1", m_lastDocsCommentSyncError), true, true);
    }
    return reviewChangeResult(m_document, m_reviewUndoIndices, m_reviewRedoIndices, true, QString(), false, true);
}

} // namespace Okular

#include "part.moc"

/* kate: replace-tabs on; indent-width 4; */

void Okular::Part::requestFedoraBuildSourceDiff(const QString &buildId)
{
    m_fedoraRepository->requestBuildSourceDiff(buildId);
}
