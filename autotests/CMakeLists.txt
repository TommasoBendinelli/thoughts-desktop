add_definitions( -DKDESRCDIR="${CMAKE_CURRENT_SOURCE_DIR}/" )

include_directories(${CMAKE_CURRENT_BINARY_DIR}/..)

macro(add_conf_unittest _source)
  set(_test ${_source} ../part/editdrawingtooldialog.cpp)
  get_filename_component(_name ${_source} NAME_WE)
  add_executable( ${_name} ${_test} )
  add_test( NAME ${_name} COMMAND ${_name} )
  ecm_mark_as_test(${_name})
  target_link_libraries( ${_name}
    Qt6::Test
    Qt6::Widgets
    Qt6::Xml
    KF6::I18n
    KF6::Completion
    KF6::WidgetsAddons
  )
endmacro ()

add_conf_unittest(editdrawingtooldialogtest.cpp)

ecm_add_test(shelltest.cpp ../shell/shellutils.cpp
    TEST_NAME "shelltest"
    LINK_LIBRARIES Qt6::Test okularcore
)

if(Poppler_Qt6_FOUND)
    if (BUILD_DESKTOP)
        ecm_add_test(parttest.cpp closedialoghelper.cpp
            TEST_NAME "parttest"
            LINK_LIBRARIES Qt6::Widgets Qt6::Test Qt6::Xml okularcore okularpart
        )
        # The Sync state machine against a scripted local server.
        ecm_add_test(syncflowtest.cpp
            TEST_NAME "syncflowtest"
            LINK_LIBRARIES Qt6::Widgets Qt6::Network Qt6::Test okularcore okularpart
        )
        ecm_add_test(htmlviewtest.cpp
            TEST_NAME "htmlviewtest"
            LINK_LIBRARIES Qt6::Widgets Qt6::Test okularcore okularpart thoughtshtmlview thoughtsplatform
        )
        # Skipped unless tools/e2e/run-document-pipeline-e2e.py provides a local server.
        ecm_add_test(documentpipelinee2etest.cpp ../shell/websiteimportdialog.cpp ../shell/reviewstore.cpp ../shell/reviewdocumenttools.cpp
            ../shell/proposalauthoring.cpp ../shell/proposallinks.cpp ../shell/thoughtscommandprotocol.cpp
            TEST_NAME "documentpipelinee2etest"
            LINK_LIBRARIES Qt6::Widgets Qt6::Network Qt6::Sql Qt6::Test Poppler::Qt6 KF6::I18n KF6::Archive okularcore okularpart
                           thoughtsdesktopsync okulardocscommentsync thoughtspublisher
        )
        # Not a unit test: tools/sync-e2e runs it against the Fedora test instance.
        add_executable(thoughts-sync-driver syncdriver.cpp)
        target_link_libraries(thoughts-sync-driver Qt6::Widgets Qt6::Test okularcore okularpart)
        set_target_properties(thoughts-sync-driver PROPERTIES MACOSX_BUNDLE FALSE)
    endif()

    ecm_add_test(visibilitytest.cpp
        TEST_NAME "visibilitytest"
        LINK_LIBRARIES Qt6::Widgets Qt6::Test okularcore
    )

    ecm_add_test(jsfunctionstest.cpp
        TEST_NAME "jsfunctionstest"
        LINK_LIBRARIES Qt6::Widgets Qt6::Test okularcore
    )

    ecm_add_test(formattest.cpp
        TEST_NAME "formattest"
        LINK_LIBRARIES Qt6::Widgets Qt6::Test okularcore
    )

    ecm_add_test(keystroketest.cpp
        TEST_NAME "keystroketest"
        LINK_LIBRARIES Qt6::Widgets Qt6::Test okularcore
    )

    ecm_add_test(signunsignedfieldtest.cpp
        TEST_NAME "signunsignedfieldtest"
        LINK_LIBRARIES Qt6::Widgets Qt6::Test okularcore Poppler::Qt6
    )
endif()

ecm_add_test(suggestedfilenametest.cpp
    TEST_NAME "suggestedfilenametest"
    LINK_LIBRARIES Qt6::Test okularcore KF6::I18n
)

ecm_add_test(documenttest.cpp
    TEST_NAME "documenttest"
    LINK_LIBRARIES Qt6::Widgets Qt6::Test Qt6::Xml okularcore KF6::ThreadWeaver thoughtsplatform
)

ecm_add_test(searchtest.cpp
    TEST_NAME "searchtest"
    LINK_LIBRARIES Qt6::Widgets Qt6::Test Qt6::Xml okularcore
)

ecm_add_test(annotationstest.cpp
    TEST_NAME "annotationstest"
    LINK_LIBRARIES Qt6::Widgets Qt6::Test Qt6::Xml okularcore
)

if(BUILD_DESKTOP)
    ecm_add_test(annotationclipboardtest.cpp
        TEST_NAME "annotationclipboardtest"
        LINK_LIBRARIES Qt6::Widgets Qt6::Test Qt6::Xml okularcore okularpart
    )
endif()

ecm_add_test(urldetecttest.cpp
    TEST_NAME "urldetecttest"
    LINK_LIBRARIES Qt6::Widgets Qt6::Test Qt6::Xml KF6::CoreAddons
)

ecm_add_test(editannotationcontentstest.cpp testingutils.cpp
    TEST_NAME "editannotationcontentstest"
    LINK_LIBRARIES Qt6::Widgets Qt6::Test Qt6::Xml okularcore
)

ecm_add_test(addremoveannotationtest.cpp testingutils.cpp
    TEST_NAME "addremoveannotationtest"
    LINK_LIBRARIES Qt6::Widgets Qt6::Test Qt6::Xml okularcore
)

ecm_add_test(translateannotationtest.cpp testingutils.cpp
    TEST_NAME "translateannotationtest"
    LINK_LIBRARIES Qt6::Widgets Qt6::Test Qt6::Xml okularcore
)

ecm_add_test(modifyannotationpropertiestest.cpp testingutils.cpp
    TEST_NAME "modifyannotationpropertiestest"
    LINK_LIBRARIES Qt6::Widgets Qt6::Test okularcore
)

ecm_add_test(editformstest.cpp
    TEST_NAME "editformstest"
    LINK_LIBRARIES Qt6::Widgets Qt6::Test okularcore
)

ecm_add_test(calculatetexttest.cpp
    TEST_NAME "calculatetexttest"
    LINK_LIBRARIES Qt6::Widgets Qt6::Test okularcore
)

ecm_add_test(check_distinguished_name_parser.cpp
    TEST_NAME "distinguishednameparser"
    LINK_LIBRARIES Qt6::Test)

if(BUILD_DESKTOP)
    ecm_add_test(textlinegeometrytest.cpp
        TEST_NAME "textlinegeometrytest"
        LINK_LIBRARIES Qt6::Gui Qt6::Test
    )
    ecm_add_test(agentproposaloverlayutilstest.cpp ../part/agentproposaloverlayutils.cpp
        TEST_NAME "agentproposaloverlayutilstest"
        LINK_LIBRARIES Qt6::Core Qt6::Test
    )

    ecm_add_test(publishedcommentanchortest.cpp
        TEST_NAME "publishedcommentanchortest"
        LINK_LIBRARIES Qt6::Test KF6::Archive okulardocscommentsync
    )

    ecm_add_test(docscommentmutationstoretest.cpp
        TEST_NAME "docscommentmutationstoretest"
        LINK_LIBRARIES Qt6::Test okulardocscommentsync
    )

    ecm_add_test(projectreadinggenerationtest.cpp
        TEST_NAME "projectreadinggenerationtest"
        LINK_LIBRARIES Qt6::Core Qt6::Test thoughtsplatform
    )

    ecm_add_test(desktopsyncstoretest.cpp
        TEST_NAME "desktopsyncstoretest"
        LINK_LIBRARIES Qt6::Core Qt6::Sql Qt6::Test thoughtsdesktopsync
    )

    ecm_add_test(defaultfoldermigrationtest.cpp
        TEST_NAME "defaultfoldermigrationtest"
        LINK_LIBRARIES Qt6::Core Qt6::Sql Qt6::Test Qt6::Widgets thoughtsdesktopsync thoughtsplatform
    )

    ecm_add_test(sourcefoldernamestest.cpp
        TEST_NAME "sourcefoldernamestest"
        LINK_LIBRARIES Qt6::Core Qt6::Test
    )

    # On-screen texts of shell/ and part/ say "the server", never Fedora.
    ecm_add_test(serverwordingtest.cpp
        TEST_NAME "serverwordingtest"
        LINK_LIBRARIES Qt6::Core Qt6::Test
    )

    ecm_add_test(sourcefoldermigrationtest.cpp
        TEST_NAME "sourcefoldermigrationtest"
        LINK_LIBRARIES Qt6::Core Qt6::Sql Qt6::Test Qt6::Widgets thoughtsdesktopsync thoughtsplatform thoughtspublisher
    )

    ecm_add_test(pdflinkoverridetest.cpp
        TEST_NAME "pdflinkoverridetest"
        LINK_LIBRARIES Qt6::Core Qt6::Sql Qt6::Test thoughtsdesktopsync
    )

    ecm_add_test(agentconversationstest.cpp
        TEST_NAME "agentconversationstest"
        LINK_LIBRARIES Qt6::Core Qt6::Sql Qt6::Test thoughtsdesktopsync
    )

    ecm_add_test(reviewcardfeedbacktest.cpp
        TEST_NAME "reviewcardfeedbacktest"
        LINK_LIBRARIES Qt6::Core Qt6::Sql Qt6::Test thoughtsdesktopsync
    )

    ecm_add_test(documentfeedbacktest.cpp
        TEST_NAME "documentfeedbacktest"
        LINK_LIBRARIES Qt6::Core Qt6::Sql Qt6::Test thoughtsdesktopsync
    )

    ecm_add_test(pdfversionstest.cpp
        TEST_NAME "pdfversionstest"
        LINK_LIBRARIES Qt6::Core Qt6::Network Qt6::Test thoughtsdesktopsync
    )

    ecm_add_test(synctransferledgertest.cpp
        TEST_NAME "synctransferledgertest"
        LINK_LIBRARIES Qt6::Core Qt6::Test thoughtsdesktopsync
    )

    ecm_add_test(frozenpublicationbundletest.cpp
        TEST_NAME "frozenpublicationbundletest"
        LINK_LIBRARIES Qt6::Core Qt6::Test thoughtsdesktopsync
    )

    ecm_add_test(documentstyleinformationtest.cpp ../shell/documentstyleinformation.cpp
        TEST_NAME "documentstyleinformationtest"
        LINK_LIBRARIES Qt6::Core Qt6::Test thoughtsplatform
    )

    ecm_add_test(websiteimporttest.cpp ../shell/websiteimportdialog.cpp
        TEST_NAME "websiteimporttest"
        LINK_LIBRARIES Qt6::Widgets Qt6::Network Qt6::Test Poppler::Qt6 KF6::I18n
    )

    ecm_add_test(commentsoverviewtest.cpp ../shell/serverlink.cpp ../shell/commentcodexlauncher.cpp ../shell/commentsoverview.cpp ../shell/appsignin.cpp ../shell/signindialog.cpp ../shell/projectsharedialog.cpp ../shell/problemreportdialog.cpp ../shell/librarysearch.cpp ../shell/websiteimportdialog.cpp ../shell/developerlog.cpp ../shell/documentstyleinformation.cpp ../shell/documentsyncpresentation.cpp ../shell/reviewoverview.cpp ../shell/reviewdraftsync.cpp ../shell/reviewfeedbackpanel.cpp ../shell/reviewcardeditor.cpp ../shell/reviewcardmerge.cpp ../shell/reviewstore.cpp ../shell/reviewdocumenttools.cpp
        TEST_NAME "commentsoverviewtest"
        LINK_LIBRARIES Qt6::Widgets Qt6::Sql Qt6::Test Poppler::Qt6 okularcore okulardocscommentsync thoughtsdesktopsync KF6::ConfigCore KF6::I18n KF6::WidgetsAddons thoughtsgitclone
    )

    ecm_add_test(htmleditiontest.cpp
        TEST_NAME "htmleditiontest"
        LINK_LIBRARIES Qt6::Core Qt6::Network Qt6::Test thoughtshtmlview
    )

    ecm_add_test(readaloudtest.cpp
        TEST_NAME "readaloudtest"
        LINK_LIBRARIES Qt6::Core Qt6::Gui Qt6::Network Qt6::Test thoughtsreadaloud
    )

    # Signing in with a username and password (protocol/app-sign-in.md): connect links, the
    # server's answers, Sign In and the Server dialog's devices against a fake server.
    ecm_add_test(appsignintest.cpp ../shell/appsignin.cpp ../shell/signindialog.cpp ../shell/projectsharedialog.cpp ../shell/problemreportdialog.cpp
        TEST_NAME "appsignintest"
        LINK_LIBRARIES Qt6::Widgets Qt6::Network Qt6::Test KF6::I18n thoughtsplatform
    )

    ecm_add_test(agentnotestest.cpp ../shell/agentnotes.cpp ../shell/agentnoteswindow.cpp ../shell/agentnotecomposer.cpp ../shell/agentnotedrawing.cpp ../shell/developerlog.cpp ../shell/serverlink.cpp
        TEST_NAME "agentnotestest"
        LINK_LIBRARIES Qt6::Widgets Qt6::Network Qt6::Test KF6::I18n thoughtsplatform
    )

    ecm_add_test(todocommandstest.cpp ../shell/todocommands.cpp ../shell/thoughtscommandprotocol.cpp
        TEST_NAME "todocommandstest"
        LINK_LIBRARIES Qt6::Core Qt6::Test thoughtsplatform
    )
    ecm_add_test(librarysearchtest.cpp ../shell/librarysearch.cpp
        TEST_NAME "librarysearchtest"
        LINK_LIBRARIES Qt6::Widgets Qt6::Sql Qt6::Test Qt6::Concurrent Poppler::Qt6 KF6::I18n thoughtsagentproposals
    )

    ecm_add_test(sourceannotationindextest.cpp ../shell/reviewdocumenttools.cpp
        TEST_NAME "sourceannotationindextest"
        LINK_LIBRARIES Qt6::Core Qt6::Gui Qt6::Test Poppler::Qt6 okulardocscommentsync thoughtsdesktopsync
    )
    ecm_add_test(reviewstoretest.cpp ../shell/reviewstore.cpp ../shell/reviewtagplan.cpp ../shell/reviewdocumenttools.cpp
        TEST_NAME "reviewstoretest"
        LINK_LIBRARIES Qt6::Core Qt6::Sql Qt6::Test thoughtsdesktopsync Qt6::Gui Poppler::Qt6 okulardocscommentsync KF6::Archive
    )

    ecm_add_test(reviewknowledgetest.cpp ../shell/reviewknowledge.cpp ../shell/reviewstore.cpp ../shell/reviewdocumenttools.cpp
        TEST_NAME "reviewknowledgetest"
        LINK_LIBRARIES Qt6::Core Qt6::Sql Qt6::Test thoughtsdesktopsync Qt6::Gui Poppler::Qt6 okulardocscommentsync KF6::Archive
    )

    ecm_add_test(reviewcardeditortest.cpp ../shell/reviewcardeditor.cpp ../shell/reviewstore.cpp ../shell/reviewdocumenttools.cpp
        TEST_NAME "reviewcardeditortest"
        LINK_LIBRARIES Qt6::Widgets Qt6::Sql Qt6::Test Poppler::Qt6 KF6::I18n okularcore thoughtsdesktopsync okulardocscommentsync
    )

    ecm_add_test(thoughtscommandprotocoltest.cpp ../shell/thoughtscommandprotocol.cpp
        TEST_NAME "thoughtscommandprotocoltest"
        LINK_LIBRARIES Qt6::Core Qt6::Test thoughtsplatform
    )
    ecm_add_test(proposalauthoringtest.cpp ../shell/proposalauthoring.cpp ../shell/proposallinks.cpp ../shell/thoughtscommandprotocol.cpp
        ../shell/reviewdocumenttools.cpp
        TEST_NAME "proposalauthoringtest"
        LINK_LIBRARIES Qt6::Core Qt6::Gui Qt6::Test Poppler::Qt6 okulardocscommentsync thoughtsdesktopsync thoughtspublisher KF6::Archive
    )
    # Decision claims, decide-proposal and Restore Proposal (protocol/agent-proposals.md
    # section 10; autotests/data/agent-proposals/decisions.json is a copy of the server's fixture).
    ecm_add_test(proposaldecisionstest.cpp ../shell/proposalauthoring.cpp ../shell/proposallinks.cpp ../shell/thoughtscommandprotocol.cpp
        ../shell/reviewdocumenttools.cpp
        TEST_NAME "proposaldecisionstest"
        LINK_LIBRARIES Qt6::Core Qt6::Gui Qt6::Network Qt6::Sql Qt6::Test Poppler::Qt6 okulardocscommentsync thoughtsdesktopsync thoughtspublisher KF6::Archive
    )
    # Accept/Reject from the proposal window, written off the UI thread.
    ecm_add_test(agentproposaldecisionqueuetest.cpp ../part/agentproposaldecisionqueue.cpp
        TEST_NAME "agentproposaldecisionqueuetest"
        LINK_LIBRARIES Qt6::Core Qt6::Sql Qt6::Test okulardocscommentsync thoughtsdesktopsync thoughtspublisher
    )
    target_include_directories(agentproposaldecisionqueuetest PRIVATE ${CMAKE_SOURCE_DIR})
    # Runs the agent-proposal spec fixtures (autotests/data/agent-proposals/cases.json,
    # a copy of the Thoughts server's protocol/fixtures/agent-proposals/cases.json).
    ecm_add_test(agentproposalconformancetest.cpp
        TEST_NAME "agentproposalconformancetest"
        LINK_LIBRARIES Qt6::Core Qt6::Test okulardocscommentsync
    )

    ecm_add_test(mainshelltest.cpp ../shell/activeparttoolbarcontroller.cpp ../shell/agentnotes.cpp ../shell/serverlink.cpp ../shell/syncstatewatch.cpp ../shell/agentnoteswindow.cpp ../shell/agentnotecomposer.cpp ../shell/agentnotedrawing.cpp ../shell/okular_main.cpp ../shell/buildterminallauncher.cpp ../shell/commentcodexlauncher.cpp ../shell/commentsoverview.cpp ../shell/appsignin.cpp ../shell/signindialog.cpp ../shell/projectsharedialog.cpp ../shell/problemreportdialog.cpp ../shell/librarysearch.cpp ../shell/websiteimportdialog.cpp ../shell/documentstyleinformation.cpp ../shell/documentsyncpresentation.cpp ../shell/reviewoverview.cpp ../shell/reviewdraftsync.cpp ../shell/reviewfeedbackpanel.cpp ../shell/reviewstore.cpp ../shell/reviewtagplan.cpp ../shell/reviewdocumenttools.cpp ../shell/shellutils.cpp ../shell/shell.cpp ../shell/syncjournal.cpp ../shell/todocommands.cpp ../shell/reviewcardeditor.cpp ../shell/reviewcardmerge.cpp ../shell/developerlog.cpp ../shell/developerlogdialog.cpp ../shell/thoughtscommandprotocol.cpp ../shell/welcomescreen.cpp ../shell/updatecheck.cpp ../shell/visualstudiocode.cpp ../shell/shell.qrc ../gui/recentitemsmodel.cpp closedialoghelper.cpp
        TEST_NAME "mainshelltest"
        LINK_LIBRARIES OpenSSL::Crypto Qt6::Concurrent Qt6::Network Qt6::Sql Qt6::Test Poppler::Qt6 okularpart okularcore okulardocscommentsync thoughtsclient thoughtsdesktopsync thoughtsgitclone
    )
    target_compile_definitions(mainshelltest PRIVATE OKULAR_BINARY="$<TARGET_FILE:thoughts>")
    # thoughts generates shell/ui_welcomescreen.h, which the test compiles. The
    # generators the test opens documents with (PDF, EPUB, ...) must load from
    # the build tree: an installed one links another copy of okularcore and
    # aborts on its unset SettingsCore.
    add_dependencies(mainshelltest thoughts)
    foreach(generator poppler epub comicbook djvu dvi fax fb kimgio md mobi tiff txt xps)
        if(TARGET okularGenerator_${generator})
            add_dependencies(mainshelltest okularGenerator_${generator})
        endif()
    endforeach()
    if(APPLE)
        enable_language(OBJCXX)
        target_sources(mainshelltest PRIVATE nativeapplicationmenutest.mm ../shell/readerchromemac.mm ../shell/visualstudiocodemac.mm)
        target_link_libraries(mainshelltest "-framework AppKit")
    else()
        target_sources(mainshelltest PRIVATE ../shell/readerchrome.cpp)
    endif()
    if(APPLE)
        ecm_add_test(macquiteventtest.mm ../shell/macquitevent.mm
            TEST_NAME "macquiteventtest"
            LINK_LIBRARIES Qt6::Widgets Qt6::Test "-framework AppKit"
        )
    endif()
    set_tests_properties(mainshelltest PROPERTIES RUN_SERIAL TRUE)
    if(TEST parttest)
        set_tests_properties(parttest PROPERTIES RUN_SERIAL TRUE)
    endif()

    if (HAVE_X11)
        target_link_libraries(mainshelltest Qt6::GuiPrivate)
    endif()
endif()

if(BUILD_DESKTOP)
    ecm_add_test(annotationtoolbartest.cpp ../shell/activeparttoolbarcontroller.cpp ../shell/agentnotes.cpp ../shell/serverlink.cpp ../shell/syncstatewatch.cpp ../shell/agentnoteswindow.cpp ../shell/agentnotecomposer.cpp ../shell/agentnotedrawing.cpp ../shell/okular_main.cpp ../shell/buildterminallauncher.cpp ../shell/commentcodexlauncher.cpp ../shell/commentsoverview.cpp ../shell/appsignin.cpp ../shell/signindialog.cpp ../shell/projectsharedialog.cpp ../shell/problemreportdialog.cpp ../shell/librarysearch.cpp ../shell/websiteimportdialog.cpp ../shell/documentstyleinformation.cpp ../shell/documentsyncpresentation.cpp ../shell/reviewoverview.cpp ../shell/reviewdraftsync.cpp ../shell/reviewfeedbackpanel.cpp ../shell/reviewstore.cpp ../shell/reviewtagplan.cpp ../shell/reviewdocumenttools.cpp ../shell/shellutils.cpp ../shell/shell.cpp ../shell/syncjournal.cpp ../shell/todocommands.cpp ../shell/reviewcardeditor.cpp ../shell/reviewcardmerge.cpp ../shell/developerlog.cpp ../shell/developerlogdialog.cpp ../shell/thoughtscommandprotocol.cpp ../shell/welcomescreen.cpp ../shell/updatecheck.cpp ../shell/visualstudiocode.cpp ../gui/recentitemsmodel.cpp closedialoghelper.cpp ../shell/welcomescreen.ui
        TEST_NAME "annotationtoolbartest"
        LINK_LIBRARIES OpenSSL::Crypto Qt6::Concurrent Qt6::Network Qt6::Sql Qt6::Test Poppler::Qt6 okularpart okulardocscommentsync thoughtsclient thoughtsdesktopsync thoughtsgitclone
    )

    if(APPLE)
        target_sources(annotationtoolbartest PRIVATE ../shell/readerchromemac.mm ../shell/visualstudiocodemac.mm)
        target_link_libraries(annotationtoolbartest "-framework AppKit")
    else()
        target_sources(annotationtoolbartest PRIVATE ../shell/readerchrome.cpp)
    endif()

    if (HAVE_X11)
        target_link_libraries(annotationtoolbartest Qt6::GuiPrivate)
    endif()
endif()

ecm_add_test(generatorstest.cpp
    TEST_NAME "generatorstest"
    LINK_LIBRARIES Qt6::Test KF6::CoreAddons okularcore
)
target_compile_definitions(generatorstest PRIVATE GENERATORS_BUILD_DIR="${CMAKE_BINARY_DIR}/generators")

ecm_add_test(signatureformtest.cpp
    TEST_NAME "signatureformtest"
    LINK_LIBRARIES Qt6::Test okularcore
)

find_package(Discount)

if(Discount_FOUND)
    ecm_add_test(markdowntest.cpp ../generators/markdown/converter.cpp
        TEST_NAME "markdowntest"
        LINK_LIBRARIES Qt6::Test okularcore KF6::I18n PkgConfig::Discount
    )
endif()

ecm_add_test(toggleactionmenutest.cpp ../part/toggleactionmenu.cpp
    TEST_NAME "toggleactionmenutest"
    LINK_LIBRARIES Qt6::Test KF6::WidgetsAddons
)

if(BUILD_DESKTOP)
    ecm_add_test(developerlogtest.cpp ../shell/developerlog.cpp ../shell/developerlogdialog.cpp
        TEST_NAME "developerlogtest"
        LINK_LIBRARIES Qt6::Test Qt6::Widgets KF6::I18n
    )
endif()

# The verified artifacts and parsed source artifacts kept in memory (part/verifiedartifactcache.h).
ecm_add_test(verifiedartifactcachetest.cpp
    TEST_NAME "verifiedartifactcachetest"
    LINK_LIBRARIES Qt6::Core Qt6::Test thoughtsplatform
)

# Files read again only when they changed (part/fileidentitycache.h).
ecm_add_test(fileidentitycachetest.cpp
    TEST_NAME "fileidentitycachetest"
    LINK_LIBRARIES Qt6::Core Qt6::Test thoughtsplatform
)

# When Thoughts may start a Sync by itself (shell/automaticsyncpolicy.h).
ecm_add_test(automaticsyncpolicytest.cpp
    TEST_NAME "automaticsyncpolicytest"
    LINK_LIBRARIES Qt6::Core Qt6::Test
)

# When a change made on this computer starts a Sync by itself (shell/localchangesync.h).
ecm_add_test(localchangesynctest.cpp
    TEST_NAME "localchangesynctest"
    LINK_LIBRARIES Qt6::Core Qt6::Test
)

# The pause between two checks for the server's work: a held request (shell/syncstatewatch.h).
ecm_add_test(syncstatewatchtest.cpp ../shell/syncstatewatch.cpp ../shell/serverlink.cpp
    TEST_NAME "syncstatewatchtest"
    LINK_LIBRARIES Qt6::Core Qt6::Network Qt6::Test thoughtsplatform
)

if(BUILD_DESKTOP)
    # The Sync journal: its worker thread, paged reads, retention and compaction.
    ecm_add_test(syncjournaltest.cpp ../shell/syncjournal.cpp ../shell/developerlog.cpp ../shell/thoughtscommandprotocol.cpp
        TEST_NAME "syncjournaltest"
        LINK_LIBRARIES Qt6::Test Qt6::Widgets Qt6::Sql KF6::I18n thoughtsplatform
    )
endif()

if(BUILD_DESKTOP)
    ecm_add_test(reviewdocumenttoolstest.cpp ../shell/reviewdocumenttools.cpp
        TEST_NAME "reviewdocumenttoolstest"
        LINK_LIBRARIES Qt6::Widgets Qt6::Test Qt6::PrintSupport Poppler::Qt6 okulardocscommentsync KF6::ConfigCore thoughtsdesktopsync
    )
    ecm_add_test(reviewsectiontest.cpp ../shell/reviewdocumenttools.cpp
        TEST_NAME "reviewsectiontest"
        LINK_LIBRARIES Qt6::Widgets Qt6::Test Poppler::Qt6 okulardocscommentsync KF6::ConfigCore KF6::Archive thoughtsdesktopsync
    )
endif()

if(BUILD_DESKTOP)
    ecm_add_test(markdownfolderimporttest.cpp
        TEST_NAME "markdownfolderimporttest"
        LINK_LIBRARIES Qt6::Core Qt6::Network Qt6::Test Qt6::Widgets thoughtsplatform thoughtsgitclone LibGit2::LibGit2
    )
endif()

ecm_add_test(htmlattachmenttest.cpp
    TEST_NAME "htmlattachmenttest"
    LINK_LIBRARIES Qt6::Test Qt6::Network okularcore
)

# Live HTML pages and snapshots (thoughts-server protocol/html-live-pages.md): stubs, origins,
# navigation, the snapshot file and pages written here.
ecm_add_test(htmllivepagetest.cpp
    TEST_NAME "htmllivepagetest"
    LINK_LIBRARIES Qt6::Test okularcore
)

ecm_add_test(documentidentitytest.cpp
    TEST_NAME "documentidentitytest"
    LINK_LIBRARIES Qt6::Core Qt6::Test thoughtsplatform
)

if(BUILD_DESKTOP)
    # Page links agents put in chats (shell/thoughtspagelink.h), parsed as the phone parses them.
    ecm_add_test(thoughtspagelinktest.cpp
        TEST_NAME "thoughtspagelinktest"
        LINK_LIBRARIES Qt6::Core Qt6::Test KF6::I18n thoughtsplatform
    )
endif()

ecm_add_test(thoughtsplatformtest.cpp
    TEST_NAME "thoughtsplatformtest"
    LINK_LIBRARIES Qt6::Core Qt6::Test thoughtsplatform
)

# Where the recall prediction token and the cards may be sent (shell/recallpredictorconfig.h).
ecm_add_test(recallpredictorconfigtest.cpp
    TEST_NAME "recallpredictorconfigtest"
    LINK_LIBRARIES Qt6::Core Qt6::Network Qt6::Test
)

# Where Open Source in VS Code finds VS Code (shell/visualstudiocode.h).
if(BUILD_DESKTOP)
    ecm_add_test(visualstudiocodetest.cpp ../shell/visualstudiocode.cpp
        TEST_NAME "visualstudiocodetest"
        LINK_LIBRARIES Qt6::Core Qt6::Test
    )
    if(APPLE)
        target_sources(visualstudiocodetest PRIVATE ../shell/visualstudiocodemac.mm)
        target_link_libraries(visualstudiocodetest "-framework AppKit")
    endif()
endif()

# What the updater requires of an installer before it may run (shell/updatecheck.cpp).
ecm_add_test(updatechecktest.cpp ../shell/updatecheck.cpp
    TEST_NAME "updatechecktest"
    LINK_LIBRARIES Qt6::Widgets Qt6::Network Qt6::Test KF6::I18n KF6::WidgetsAddons KF6::ConfigCore thoughtsplatform
)

# What Thoughts does with a note attachment's server-chosen name and type (part/noteattachment.h).
ecm_add_test(noteattachmenttest.cpp
    TEST_NAME "noteattachmenttest"
    LINK_LIBRARIES Qt6::Core Qt6::Test thoughtsplatform
)

ecm_add_test(thoughtsserveraddresstest.cpp
    TEST_NAME "thoughtsserveraddresstest"
    LINK_LIBRARIES Qt6::Core Qt6::Test thoughtsplatform
)

# The PowerShell scripts of the Windows build-output viewer quote file names
# that Thoughts does not choose; the quoting is the same on every system.
ecm_add_test(powershellquotetest.cpp ../shell/buildterminallauncher.cpp
    TEST_NAME "powershellquotetest"
    LINK_LIBRARIES Qt6::Core Qt6::Test KF6::I18n thoughtsplatform
)

ecm_add_test(readerdisplaytitletest.cpp
    TEST_NAME "readerdisplaytitletest"
    LINK_LIBRARIES Qt6::Core Qt6::Test Qt6::Concurrent Qt6::Widgets thoughtsdesktopsync
)

ecm_add_test(projectconfigurationcachetest.cpp
    TEST_NAME "projectconfigurationcachetest"
    LINK_LIBRARIES Qt6::Core Qt6::Test Qt6::Concurrent thoughtsplatform
)

ecm_add_test(sourceownertest.cpp
    TEST_NAME "sourceownertest"
    LINK_LIBRARIES Qt6::Core Qt6::Test
)

# A docs folder's HTML pages kept in step with Fedora's page list (tools/publisher/htmlpages.cpp).
ecm_add_test(htmlpagessynctest.cpp
    TEST_NAME "htmlpagessynctest"
    LINK_LIBRARIES Qt6::Core Qt6::Network Qt6::Test thoughtspublisher
)

# Server-chosen ids the publisher turns into paths, and its redirects (tools/publisher/serverids.h, sourceapi.cpp).
ecm_add_test(publisherserveridstest.cpp
    TEST_NAME "publisherserveridstest"
    LINK_LIBRARIES Qt6::Core Qt6::Network Qt6::Test thoughtspublisher
)

# Floating-point JSON numbers read to the same doubles as Python's json.loads (tools/publisher/pyjson.cpp).
ecm_add_test(pyjsonfloattest.cpp
    TEST_NAME "pyjsonfloattest"
    LINK_LIBRARIES Qt6::Core Qt6::Test thoughtspublisher
)

# One reading of the Library's Project files per publication (tools/publisher/buildinputs.cpp).
ecm_add_test(publishertargetscopetest.cpp
    TEST_NAME "publishertargetscopetest"
    LINK_LIBRARIES Qt6::Core Qt6::Test thoughtspublisher
)

# The server's source tree kept between the publisher runs of a Sync (tools/publisher/sourcepublisher.cpp).
ecm_add_test(publishersourcetreetest.cpp
    TEST_NAME "publishersourcetreetest"
    LINK_LIBRARIES Qt6::Core Qt6::Network Qt6::Test thoughtspublisher
)

# Sync's three-way text merge with git's compiled-in xdiff (tools/publisher/xdiff).
ecm_add_test(sourcemergetest.cpp
    TEST_NAME "sourcemergetest"
    LINK_LIBRARIES Qt6::Core Qt6::Test thoughtspublisher
)

ecm_add_test(syncstatuslabelstest.cpp
    TEST_NAME "syncstatuslabelstest"
    LINK_LIBRARIES Qt6::Core Qt6::Test KF6::I18n
)

ecm_add_test(documentsyncpresentationtest.cpp ../shell/documentsyncpresentation.cpp
    TEST_NAME "documentsyncpresentationtest"
    LINK_LIBRARIES Qt6::Core Qt6::Test KF6::I18n thoughtsplatform
)

# The annotation tool definitions are read through QStandardPaths, which on
# macOS cannot be pointed at an install prefix but does search the
# executable's directory.
if(APPLE)
    file(COPY ${CMAKE_SOURCE_DIR}/part/data/tools.xml ${CMAKE_SOURCE_DIR}/part/data/toolsQuick.xml ${CMAKE_SOURCE_DIR}/part/data/drawingtools.xml
        DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/okular)
endif()

# testhome.cpp makes every test offscreen by default; the quit-event test needs the real
# macOS platform to receive Apple events.
if(TEST macquiteventtest)
    set_property(TEST macquiteventtest APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=cocoa")
endif()

# Keep this last: every test runs with a temporary home (see testhome.cpp).
get_property(_autotest_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_autotest_target IN LISTS _autotest_targets)
    get_target_property(_autotest_type ${_autotest_target} TYPE)
    if(_autotest_type STREQUAL "EXECUTABLE" AND NOT _autotest_target STREQUAL "thoughts-sync-driver")
        target_sources(${_autotest_target} PRIVATE testhome.cpp)
    endif()
endforeach()
