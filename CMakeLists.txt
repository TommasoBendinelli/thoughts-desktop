cmake_minimum_required(VERSION 3.22)

# KDE Application Version, managed by release script
set (RELEASE_SERVICE_VERSION_MAJOR "26")
set (RELEASE_SERVICE_VERSION_MINOR "11")
set (RELEASE_SERVICE_VERSION_MICRO "70")
set (RELEASE_SERVICE_VERSION "${RELEASE_SERVICE_VERSION_MAJOR}.${RELEASE_SERVICE_VERSION_MINOR}.${RELEASE_SERVICE_VERSION_MICRO}")

project(okular VERSION ${RELEASE_SERVICE_VERSION})

set(QT_REQUIRED_VERSION "6.6.0") # Remember to update the QT_DEPRECATED_WARNINGS_SINCE below
set(KF_REQUIRED_VERSION "5.240.0") # Remember to update the KF_DEPRECATED_WARNINGS_SINCE below

set(OKULAR_UI "" CACHE STRING "Which Okular user interface to build. Possible values: desktop, mobile, both. Default: desktop (except on Android, where it is 'mobile')")

set(FORCE_NOT_REQUIRED_DEPENDENCIES "" CACHE STRING "List (semicolon-separated) of dependencies that will be downgraded from REQUIRED to RECOMMENDED")

find_package(ECM ${KF_REQUIRED_VERSION} CONFIG REQUIRED)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${ECM_MODULE_PATH})

include(ECMInstallIcons)
include(ECMSetupVersion)
include(ECMOptionalAddSubdirectory)
include(GenerateExportHeader)
include(FeatureSummary)
include(ECMAddAppIcon)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(KDEInstallDirs)
include(KDECMakeSettings)
include(ECMAddTests)
include(ECMMarkNonGuiExecutable)
include(ECMAddAppIcon)
include(CMakePackageConfigHelpers)
include(ECMSetupQtPluginMacroNames)
include(ECMAddAndroidApk)

if (ANDROID AND (FORCE_NOT_REQUIRED_DEPENDENCIES STREQUAL ""))
    set(FORCE_NOT_REQUIRED_DEPENDENCIES "KF6Wallet;KF6DocTools;Qt6Qml;TIFF;LibSpectre;KExiv2Qt6;DjVuLibre;EPub;Discount;")
endif()

function(set_okular_optional_package_properties _name _props)
    if(NOT "${_props}" STREQUAL "PROPERTIES")
        message(FATAL_ERROR "PROPERTIES keyword is missing in set_okular_optional_package_properties() call.")
    endif()

    set(options) # none
    set(oneValueArgs DESCRIPTION URL PURPOSE)
    set(multiValueArgs) # none

    CMAKE_PARSE_ARGUMENTS(_SPP "${options}" "${oneValueArgs}" "${multiValueArgs}"  ${ARGN})

    if(_SPP_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "Unknown keywords given to set_okular_optional_package_properties(): \"${_SPP_UNPARSED_ARGUMENTS}\"")
    endif()

    set(DEPENDENCY_TYPE "REQUIRED")
    if (${_name} IN_LIST FORCE_NOT_REQUIRED_DEPENDENCIES)
        set(DEPENDENCY_TYPE "RECOMMENDED")
    endif()
    set_package_properties(${_name} PROPERTIES
        TYPE ${DEPENDENCY_TYPE}
        DESCRIPTION ${_SPP_DESCRIPTION}
        URL ${_SPP_URL}
        PURPOSE "${_SPP_PURPOSE} You can make the dependency optional adding ${_name} to the FORCE_NOT_REQUIRED_DEPENDENCIES cmake option"
    )
endfunction()

if(OKULAR_UI STREQUAL "")
    if(ANDROID)
        set(OKULAR_UI "mobile")
    else()
        set(OKULAR_UI "desktop")
    endif()
endif()

if(OKULAR_UI STREQUAL "desktop" OR OKULAR_UI STREQUAL "both")
    set(BUILD_DESKTOP ON)
else()
    set(BUILD_DESKTOP OFF)
endif()
if(OKULAR_UI STREQUAL "mobile" OR OKULAR_UI STREQUAL "both")
    include(ECMQmlModule)
    set(BUILD_MOBILE ON)
else()
    set(BUILD_MOBILE OFF)
endif()

# append the plugins from the install tree folder to the QT_PLUGIN_PATH
if(UNIX)
    set(ENV{QT_PLUGIN_PATH} "$ENV{QT_PLUGIN_PATH}:${KDE_INSTALL_FULL_QTPLUGINDIR}")
elseif(WIN32)
    set(ENV{QT_PLUGIN_PATH} "$ENV{QT_PLUGIN_PATH};${KDE_INSTALL_FULL_QTPLUGINDIR}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

ecm_setup_version(${PROJECT_VERSION}
                  VARIABLE_PREFIX OKULAR
                  VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/core/version.h"
                  PACKAGE_VERSION_FILE "${CMAKE_CURRENT_BINARY_DIR}/Okular6ConfigVersion.cmake")

find_package(Qt6 ${QT_REQUIRED_VERSION} CONFIG REQUIRED COMPONENTS Core Concurrent Network Test Widgets PrintSupport Sql Svg Xml)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

find_package(KF6 ${KF_REQUIRED_VERSION} REQUIRED COMPONENTS
    Archive
    Bookmarks
    Completion
    Config
    ConfigWidgets
    CoreAddons
    I18n
    KIO
    ThreadWeaver
    WindowSystem
    XmlGui
    IconThemes
)

if (NOT WIN32 AND NOT HAIKU)
    find_package(KF6Wallet ${KF_REQUIRED_VERSION})
    set_okular_optional_package_properties(KF6Wallet PROPERTIES
    PURPOSE "Required for document storing passwords in secure wallets.")
endif()
if(KF6Wallet_FOUND)
    set(HAVE_KWALLET 1)
else()
    set(HAVE_KWALLET 0)
endif()

find_package(KF6DocTools ${KF_REQUIRED_VERSION})
set_okular_optional_package_properties(KF6DocTools PROPERTIES
    PURPOSE "Required for compiling and installing the user documentation.")

find_package(Qt6Qml)
set_okular_optional_package_properties(Qt6Qml PROPERTIES
    PURPOSE "Required for supporting JavaScript in PDF documents")
if(TARGET Qt6::Qml)
    set(HAVE_JS 1)
else()
    set(HAVE_JS 0)
endif()

if (BUILD_DESKTOP)
    # shall we use DBus?
    # enabled per default on Linux & BSD systems
    set(USE_DBUS_DEFAULT OFF)
    if(UNIX AND NOT APPLE AND NOT ANDROID AND NOT HAIKU)
        set(USE_DBUS_DEFAULT ON)
    endif()
    option(USE_DBUS "Build components using DBus (used for interprocess communication, external open in same instance and various presentation related bits)" ${USE_DBUS_DEFAULT})
    if(USE_DBUS)
        find_package(Qt6DBus REQUIRED)
        set(HAVE_DBUS 1)
    endif()

    find_package(KF6 ${KF_REQUIRED_VERSION} REQUIRED COMPONENTS Parts ColorScheme Crash IconThemes TextWidgets WidgetsAddons)

    find_package(KF6Purpose)
    set_okular_optional_package_properties(KF6Purpose PROPERTIES
        DESCRIPTION "A framework for services and actions integration"
        PURPOSE "Required for enabling the share menu.")
    if (KF6Purpose_FOUND)
        set(HAVE_PURPOSE 1)
    else()
        set(HAVE_PURPOSE 0)
    endif()

    find_package(Qt6TextToSpeech ${QT_REQUIRED_VERSION} CONFIG)
    set_okular_optional_package_properties(Qt6TextToSpeech PROPERTIES
        PURPOSE "Enables speech features.")
    if (Qt6TextToSpeech_FOUND)
        set(HAVE_SPEECH 1)
    else()
        set(HAVE_SPEECH 0)
    endif()

    # Read Aloud plays the cloud voice's MP3 audio (from OpenRouter) with QtMultimedia.
    find_package(Qt6Multimedia ${QT_REQUIRED_VERSION} CONFIG)
    set_okular_optional_package_properties(Qt6Multimedia PROPERTIES
        PURPOSE "Plays the cloud voice of Read Aloud.")
endif()

if (BUILD_MOBILE)
    find_package(Qt6 ${QT_REQUIRED_VERSION} CONFIG REQUIRED COMPONENTS Qml Quick)
    find_package(KF6Kirigami2)
    set_package_properties(KF6Kirigami2 PROPERTIES
        DESCRIPTION "A QtQuick based components set"
        PURPOSE "Required at runtime by the mobile app"
        TYPE RUNTIME
    )
    find_package(KF6KirigamiAddons)
    set_package_properties(KF6KirigamiAddons PROPERTIES
        DESCRIPTION "A set of widgets for Kirigami along with  code to support them"
        PURPOSE "Required at runtime by the mobile app"
        TYPE RUNTIME
    )

    qt_policy(SET QTP0001 NEW)
endif()

if(NOT WIN32 AND NOT ANDROID AND NOT APPLE AND NOT HAIKU)
    if (Qt6Gui_VERSION VERSION_GREATER_EQUAL "6.10.0")
        find_package(Qt6GuiPrivate ${REQUIRED_QT_VERSION} REQUIRED)
    else()
        find_package(Qt6Gui REQUIRED COMPONENTS Private)
    endif()
    set(HAVE_X11 TRUE)
else()
    set(HAVE_X11 FALSE)
endif()
find_package(Phonon4Qt6 CONFIG)
set_okular_optional_package_properties(Phonon4Qt6 PROPERTIES
    PURPOSE "Required for documents with audio or video elements")
if (Phonon4Qt6_FOUND)
    set(HAVE_PHONON 1)
else()
    set(HAVE_PHONON 0)
endif()

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR}/cmake/modules)

find_package(ZLIB REQUIRED)

find_package(Poppler "24.08.0" COMPONENTS Qt6)
set_okular_optional_package_properties(Poppler PROPERTIES
        PURPOSE "Support for PDF files in okular.")

find_package(Freetype)
set_okular_optional_package_properties(Freetype PROPERTIES
        DESCRIPTION  "A font rendering engine"
        URL "https://www.freetype.org"
        PURPOSE "Provides freetype font support in the okular DVI generator.")

find_package(TIFF)
set_okular_optional_package_properties(TIFF PROPERTIES
        DESCRIPTION  "A library for reading and writing TIFF formatted files,"
        URL "http://www.libtiff.org"
        PURPOSE "Support for TIFF files in okular.")

set(LIBSPECTRE_MINIMUM_VERSION "0.2")
find_package(LibSpectre "${LIBSPECTRE_MINIMUM_VERSION}")
set_okular_optional_package_properties(LibSpectre PROPERTIES
        DESCRIPTION  "A PostScript rendering library"
        URL "https://libspectre.freedesktop.org"
        PURPOSE "Support for PS files in okular.")

find_package(KExiv2Qt6 CONFIG)
set_okular_optional_package_properties(KExiv2Qt6 PROPERTIES
        DESCRIPTION  "Wrapper around Exiv2 library"
        URL "https://commits.kde.org/libkexiv2"
        PURPOSE "Support for exif rotation in image files.")

find_package(DjVuLibre "3.5.28")
set_okular_optional_package_properties(DjVuLibre PROPERTIES
        DESCRIPTION  "A library for dealing with DjVu formatted files"
        URL "https://djvulibre.djvuzone.org"
        PURPOSE "Support for DjVu files in okular.")

find_package(EPub)
set_okular_optional_package_properties(EPub PROPERTIES
        DESCRIPTION  "A library for reading EPub documents"
        URL "http://sourceforge.net/projects/ebook-tools"
        PURPOSE "Support for EPub documents in Okular.")

find_package(QMobipocket6 "2" CONFIG)
set_okular_optional_package_properties(QMobiPocket6 PROPERTIES
        DESCRIPTION  "A library for reading Mobipocket documents"
        URL "https://commits.kde.org/kdegraphics-mobipocket"
        PURPOSE "Support for Mobipocket documents in Okular.")

find_package(Discount)
set_okular_optional_package_properties(Discount PROPERTIES
        DESCRIPTION "A library that gives you formatting functions suitable for marking down entire documents or lines of text"
        URL "https://www.pell.portland.or.us/~orc/Code/discount/"
        PURPOSE "Support for Markdown documents in Okular.")

add_definitions(-DTRANSLATION_DOMAIN="okular")
add_definitions(-DQT_DEPRECATED_WARNINGS_SINCE=0x060600)
add_definitions(-DKF_DEPRECATED_WARNINGS_SINCE=0x05F000)

if (Poppler_VERSION VERSION_LESS 24.10.0)
    add_definitions(-DHAVE_NEW_SIGNATURE_API=0)
else()
    add_definitions(-DHAVE_NEW_SIGNATURE_API=1)
endif()

if (Poppler_VERSION VERSION_LESS 24.12.0)
    add_definitions(-DHAVE_AUTOMATIC_SIGNATURE_FONT_SIZE=0)
else()
    add_definitions(-DHAVE_AUTOMATIC_SIGNATURE_FONT_SIZE=1)
endif()

# Build-time Thoughts settings, written to config-thoughts.h. A new
# installation starts with THOUGHTS_DEFAULT_SERVER_URL and saves it; from then
# on the saved address wins (File › Server…, thoughtsctl set-server), and
# THOUGHTS_SERVER_URL overrides it for one process. THOUGHTS_LEGACY_SERVER_URL
# is for builds whose earlier versions had a fixed address: an installation that
# synchronized before it saved one keeps that address.
set(THOUGHTS_DEFAULT_SERVER_URL "https://thoughts.example.org" CACHE STRING "Server address a new installation starts with")
set(THOUGHTS_LEGACY_SERVER_URL "" CACHE STRING "Earlier fixed server address that installations which already synchronized keep (empty: none)")
if(NOT THOUGHTS_DEFAULT_SERVER_URL MATCHES "^https://")
    message(FATAL_ERROR "THOUGHTS_DEFAULT_SERVER_URL must be an https:// address")
endif()
if(THOUGHTS_LEGACY_SERVER_URL AND NOT THOUGHTS_LEGACY_SERVER_URL MATCHES "^https://")
    message(FATAL_ERROR "THOUGHTS_LEGACY_SERVER_URL must be empty or an https:// address")
endif()
foreach(_thoughts_setting THOUGHTS_DEFAULT_SERVER_URL THOUGHTS_LEGACY_SERVER_URL)
    string(FIND "${${_thoughts_setting}}" "\"" _thoughts_quote)
    string(FIND "${${_thoughts_setting}}" "\\" _thoughts_backslash)
    if(NOT _thoughts_quote EQUAL -1 OR NOT _thoughts_backslash EQUAL -1)
        message(FATAL_ERROR "${_thoughts_setting} must not contain quotes or backslashes")
    endif()
endforeach()
configure_file(config-thoughts.h.cmake ${CMAKE_BINARY_DIR}/thoughts-config/config-thoughts.h @ONLY)

include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${PHONON_INCLUDES} core/synctex ${CMAKE_BINARY_DIR}/core ${CMAKE_BINARY_DIR}/thoughts-config)

# Thoughts' largest sources exceed MSVC's default object-file section limit.
if(MSVC)
    add_compile_options(/bigobj)
endif()

# Operating-system services (secrets, file identity, directory links) behind
# one Qt-only interface, so Windows headers stay out of shared headers, and
# the server address that goes with this computer's device token.
add_library(thoughtsplatform STATIC part/thoughtsplatform.cpp part/thoughtsserveraddress.cpp part/libraryswitch.cpp)
set_target_properties(thoughtsplatform PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(thoughtsplatform PUBLIC Qt6::Core)
if(WIN32)
    target_link_libraries(thoughtsplatform PRIVATE advapi32 user32)
endif()
if(APPLE)
    # The computer's name (SCDynamicStoreCopyComputerName), which a signed-in Mac is listed by.
    target_link_libraries(thoughtsplatform PRIVATE "-framework SystemConfiguration" "-framework CoreFoundation")
endif()

if(BUILD_DESKTOP)
    # Agent proposals in sources (protocol/agent-proposals.md) and the decision
    # claims of section 10, shared by the app, thoughtsctl and the publisher.
    add_library(thoughtsagentproposals STATIC
        shell/agentproposalparser.cpp
        shell/proposaldecisions.cpp
    )
    set_target_properties(thoughtsagentproposals PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(thoughtsagentproposals PUBLIC Qt6::Core Qt6::Sql thoughtsplatform)
    # Claims apply only to sources Sync manages (tools/publisher/managedsource.h).
    # The publisher uses the claims in turn; CMake links the static cycle.
    target_link_libraries(thoughtsagentproposals PRIVATE thoughtspublisher)

    add_library(okulardocscommentsync STATIC
        part/docscommentmutationstore.cpp
        part/docscommentsynchronizer.cpp
        part/sourceannotationindex.cpp
    )
    set_target_properties(okulardocscommentsync PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(okulardocscommentsync PUBLIC Qt6::Core Qt6::Sql KF6::Archive okularcore thoughtsplatform thoughtsagentproposals)

    # The person's own Claude Code: where claude is, and Set up Claude Code, which registers the
    # Thoughts tools an installation brings (Settings > Agent prompts; the app's waker supervisor).
    add_library(thoughtsclaudecode STATIC part/claudecode.cpp)
    set_target_properties(thoughtsclaudecode PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(thoughtsclaudecode PUBLIC Qt6::Core PRIVATE KF6::I18n)

    # Import sources › From Git repository clones with libgit2, so no installed Git
    # is needed (GPL-2.0 with the linking exception). 1.7 adds server timeouts.
    find_package(LibGit2 1.7 REQUIRED)
    add_library(thoughtsgitclone STATIC part/gitclone.cpp part/gitclonedialog.cpp)
    set_target_properties(thoughtsgitclone PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(thoughtsgitclone PUBLIC Qt6::Widgets PRIVATE Qt6::Network LibGit2::LibGit2)

    if(Poppler_Qt6_FOUND)
        add_executable(thoughts-comment-sync
            tools/thoughts-comment-sync.cpp
        )
        set_target_properties(thoughts-comment-sync PROPERTIES MACOSX_BUNDLE FALSE)
        ecm_mark_nongui_executable(thoughts-comment-sync)
        target_link_libraries(thoughts-comment-sync
            okulardocscommentsync
            Poppler::Qt6
            Qt6::Core
        )
        install(TARGETS thoughts-comment-sync ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
    endif()
endif()

option(BUILD_COVERAGE "Build the project with gcov support" OFF)

if(BUILD_COVERAGE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-arcs -ftest-coverage")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -lgcov")
endif()

add_subdirectory( icons ) # an own directory for multi-size icons retrieved by KIconThemeLoader
add_subdirectory( part )
if(BUILD_DESKTOP)
    add_subdirectory( tools/publisher )
    add_subdirectory( shell )
endif()
add_subdirectory( generators )

if(BUILD_MOBILE)
    add_subdirectory( mobile )
endif()

if(BUILD_TESTING)
   add_subdirectory( autotests )
endif()

if(KF6DocTools_FOUND)
    add_subdirectory(doc)
endif()

include(OkularConfigureChecks.cmake)

if(NOT WIN32)
 set(MATH_LIB m)
else(NOT WIN32)
 set(MATH_LIB)
endif(NOT WIN32)

# okularcore

set(okularcore_SRCS
   core/action.cpp
   core/annotations.cpp
   core/area.cpp
   core/audioplayer.cpp
   core/bookmarkmanager.cpp
   core/chooseenginedialog.cpp
   core/document.cpp
   core/htmlattachment.cpp
   core/htmllivepage.cpp
   core/documentcommands.cpp
   core/fontinfo.cpp
   core/form.cpp
   core/generator.cpp
   core/generator_p.cpp
   core/misc.cpp
   core/movie.cpp
   core/observer.cpp
   core/debug.cpp
   core/page.cpp
   core/pagecontroller.cpp
   core/pagesize.cpp
   core/pagetransition.cpp
   core/rotationjob.cpp
   core/scripter.cpp
   core/sound.cpp
   core/sourcereference.cpp
   core/textdocumentgenerator.cpp
   core/textdocumentsettings.cpp
   core/textpage.cpp
   core/tilesmanager.cpp
   core/utils.cpp
   core/view.cpp
   core/fileprinter.cpp
   core/printoptionswidget.cpp
   core/signatureutils.cpp
   core/script/event.cpp
   core/synctex/synctex_parser.c
   core/synctex/synctex_parser_utils.c
)
qt_add_resources(okularcore_SRCS
   core/script/builtin.qrc
)

ki18n_wrap_ui(okularcore_SRCS
   core/textdocumentsettings.ui
)

install( FILES
           core/action.h
           core/annotations.h
           core/area.h
           core/document.h
           core/fontinfo.h
           core/form.h
           core/generator.h
           core/global.h
           core/page.h
           core/pagesize.h
           core/pagetransition.h
           core/signatureutils.h
           core/sound.h
           core/sourcereference.h
           core/textdocumentgenerator.h
           core/textdocumentsettings.h
           core/textpage.h
           core/tile.h
           core/utils.h
           core/fileprinter.h
           core/printoptionswidget.h
           core/observer.h
           ${CMAKE_CURRENT_BINARY_DIR}/core/version.h
           ${CMAKE_CURRENT_BINARY_DIR}/core/okularcore_export.h
           ${CMAKE_CURRENT_BINARY_DIR}/settings_core.h
         DESTINATION ${KDE_INSTALL_INCLUDEDIR}/okular/core COMPONENT Devel)

install( FILES
           interfaces/configinterface.h
           interfaces/guiinterface.h
           interfaces/printinterface.h
           interfaces/saveinterface.h
           interfaces/viewerinterface.h
         DESTINATION ${KDE_INSTALL_INCLUDEDIR}/okular/interfaces COMPONENT Devel)

ki18n_wrap_ui(okularcore_SRCS
   core/chooseenginewidget.ui
)

kconfig_add_kcfg_files(okularcore_SRCS GENERATE_MOC conf/settings_core.kcfgc)

add_library(okularcore SHARED ${okularcore_SRCS})
if(BUILD_DESKTOP)
    target_sources(okularcore PRIVATE part/sourceannotationindexstore.cpp)
endif()
generate_export_header(okularcore BASE_NAME okularcore EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/core/okularcore_export.h")
target_include_directories(okularcore PUBLIC "$<INSTALL_INTERFACE:${KDE_INSTALL_INCLUDEDIR}>")

if (ANDROID)
    set(fileName ${CMAKE_BINARY_DIR}/Okular5Core-android-dependencies.xml)
    file(WRITE "${fileName}" "<rules><dependencies><lib name='Okular5Core'><depends>\n"
                                "<bundled file='${KDE_INSTALL_PLUGINDIR}/okular/generators' />\n"
                             "</depends></lib></dependencies></rules>\n")
    install(FILES ${fileName} DESTINATION ${KDE_INSTALL_LIBDIR})
endif()


# Special handling for linking okularcore on OSX/Apple
IF(APPLE)
    SET(OKULAR_IOKIT "-framework IOKit" CACHE STRING "Apple IOKit framework")
ENDIF(APPLE)

# Extra library needed by imported synctex code on Windows
if(WIN32)
    set(SHLWAPI shlwapi)
endif(WIN32)

target_link_libraries(okularcore
PRIVATE
    ${OKULAR_IOKIT}
    ${SHLWAPI}
    thoughtsplatform
    KF6::Archive
    KF6::KIOCore
    KF6::KIOWidgets
    KF6::I18n
    KF6::ThreadWeaver
    KF6::Bookmarks
    Qt6::Svg
    Qt6::Sql
    Qt6::Network
    ${MATH_LIB}
    ZLIB::ZLIB
PUBLIC  # these are included from the installed headers
    KF6::CoreAddons
    KF6::XmlGui
    KF6::ConfigGui
    Qt6::PrintSupport
    Qt6::Widgets
)
if (Phonon4Qt6_FOUND)
    target_link_libraries(okularcore PRIVATE Phonon::phonon4qt6)
endif()


if (KF6Wallet_FOUND)
    target_link_libraries(okularcore PRIVATE KF6::Wallet)
endif()
if (TARGET Qt6::Qml)
    target_sources(okularcore PRIVATE
            core/script/executor_js.cpp
            core/script/js_app.cpp
            core/script/js_console.cpp
            core/script/js_data.cpp
            core/script/js_display.cpp
            core/script/js_document.cpp
            core/script/js_field.cpp
            core/script/js_fullscreen.cpp
            core/script/js_field.cpp
            core/script/js_spell.cpp
            core/script/js_util.cpp
            core/script/js_event.cpp
            core/script/js_ocg.cpp
            core/script/js_global.cpp
    )
    target_link_libraries(okularcore PRIVATE Qt6::Qml)
endif()

set_target_properties(okularcore PROPERTIES VERSION 4.0.0 SOVERSION 4 OUTPUT_NAME Okular6Core EXPORT_NAME Core)

install(TARGETS okularcore EXPORT Okular6Targets ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})

install(FILES conf/okular.kcfg DESTINATION ${KDE_INSTALL_KCFGDIR})
install(FILES conf/okular_core.kcfg DESTINATION ${KDE_INSTALL_KCFGDIR})

if(BUILD_DESKTOP)
    find_package(zstd CONFIG QUIET)
    set(THOUGHTS_SERVER_SDK_ROOT "${CMAKE_SOURCE_DIR}/../thoughts-server/sdk/cpp" CACHE PATH "Thoughts standalone client SDK")
    if(EXISTS "${THOUGHTS_SERVER_SDK_ROOT}/CMakeLists.txt")
        add_subdirectory("${THOUGHTS_SERVER_SDK_ROOT}" "${CMAKE_BINARY_DIR}/thoughts-client" EXCLUDE_FROM_ALL)
    else()
        message(FATAL_ERROR "Thoughts client SDK not found at ${THOUGHTS_SERVER_SDK_ROOT}")
    endif()

  add_library(thoughtsdesktopsync STATIC
    part/agentconversations.cpp
    part/defaultfoldermigration.cpp
    part/sourcefoldermigration.cpp
    part/desktopsyncstore.cpp
    part/docschangemonitor.cpp
    part/feedbackversion.cpp
    part/frozenpublicationbundle.cpp
    part/linkoverridestore.cpp
    part/pdfversions.cpp
    part/reviewcardfeedback.cpp
    part/documentfeedback.cpp
    part/readingoverlays.cpp
    part/sourcechangetracker.cpp
    part/synctransferledger.cpp
  )
  target_link_libraries(thoughtsdesktopsync
    PUBLIC Qt6::Core thoughtsclient thoughtsplatform
    PRIVATE Qt6::Concurrent Qt6::Sql thoughtspublisher
  )
  if(APPLE)
    target_link_libraries(thoughtsdesktopsync PUBLIC "-framework CoreServices")
  endif()

  # Read Aloud: text preparation, the OpenRouter speech client and key store, playback and the
  # system-voice fallback, without widgets so tests can drive it.
  add_library(thoughtsreadaloud STATIC
    part/readaloud.cpp
    part/readaloudtext.cpp
  )
  set_target_properties(thoughtsreadaloud PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_link_libraries(thoughtsreadaloud PUBLIC Qt6::Core Qt6::Network PRIVATE thoughtsplatform)
  if(Qt6Multimedia_FOUND)
    target_link_libraries(thoughtsreadaloud PRIVATE Qt6::Multimedia)
    target_compile_definitions(thoughtsreadaloud PRIVATE THOUGHTS_HAVE_QTMULTIMEDIA=1)
  endif()
  if(Qt6TextToSpeech_FOUND)
    target_link_libraries(thoughtsreadaloud PRIVATE Qt6::TextToSpeech)
    target_compile_definitions(thoughtsreadaloud PRIVATE THOUGHTS_HAVE_TEXTTOSPEECH=1)
  endif()

  # HTML editions (thoughts-html-view-SPEC.md): edition cache, loopback server and the reader
  # model, without widgets so tests can drive them. The web view itself (Qt WebView: WKWebView on
  # macOS, WebView2 on Windows) is optional: without it the HTML view says it is not available.
  find_package(Qt6WebView ${QT_REQUIRED_VERSION} CONFIG QUIET)
  option(THOUGHTS_HTML_VIEW "Build the HTML view with Qt WebView" ${Qt6WebView_FOUND})
  if(WIN32)
    # Live pages, embedded pages and HTML editions on Windows (part/*_win.cpp): Microsoft Edge
    # WebView2, from the build/native folder of Microsoft's Microsoft.Web.WebView2 NuGet package
    # (include/, x64/). Without it they open in the browser.
    set(THOUGHTS_WEBVIEW2_SDK "" CACHE PATH "build/native folder of the Microsoft.Web.WebView2 NuGet package (HTML pages)")
  endif()
  add_library(thoughtshtmlview STATIC
    part/htmledition.cpp
    part/htmleditionserver.cpp
  )
  set_target_properties(thoughtshtmlview PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_link_libraries(thoughtshtmlview PUBLIC Qt6::Core Qt6::Network PRIVATE thoughtsplatform)

  # okularpart
  set(okularpart_SRCS
    gui/certificatemodel.cpp
    gui/debug_ui.cpp
    gui/guiutils.cpp
    gui/pagepainter.cpp
    gui/signatureguiutils.cpp
    gui/signaturemodel.cpp
    gui/tocmodel.cpp
    part/preferencesdialog.cpp
    part/dlgaccessibility.cpp
    part/dlgagentprompts.cpp
    part/dlgreadaloud.cpp
    part/dlgdebug.cpp
    part/dlgeditor.cpp
    part/dlggeneral.cpp
    part/dlgannotations.cpp
    part/dlgperformance.cpp
    part/dlgpresentation.cpp
    part/editannottooldialog.cpp
    part/editdrawingtooldialog.cpp
    part/widgetannottools.cpp
    part/widgetconfigurationtoolsbase.cpp
    part/widgetdrawingtools.cpp
    part/commentfragmentgroup.cpp
    part/fedorarepository.cpp
    part/part.cpp
    part/commentservice.cpp
    part/extensions.cpp
    part/embeddedfilesdialog.cpp
    part/actionbar.cpp
    part/agentproposaldecisionqueue.cpp
    part/agentproposalhoverbar.cpp
    part/agentproposaloverlayutils.cpp
    part/agentproposalwindow.cpp
    part/annotationactionhandler.cpp
    part/annotwindow.cpp
    part/annotationmodel.cpp
    part/annotationpopup.cpp
    part/annotationpropertiesdialog.cpp
    part/annotationproxymodels.cpp
    part/annotationtools.cpp
    part/annotationwidgets.cpp
    part/bookmarklist.cpp
    part/certificateviewer.cpp
    part/colormodemenu.cpp
    part/cursorwraphelper.cpp
    part/drawingtoolactions.cpp
    part/fileprinterpreview.cpp
    part/findbar.cpp
    part/formwidgets.cpp
    part/kleopatraintegration.h
    part/kleopatraintegration.cpp
    part/ktreeviewsearchline.cpp
    part/latexrenderer.cpp
    part/minibar.cpp
    part/okmenutitle.cpp
    part/pageitemdelegate.cpp
    part/pagesizelabel.cpp
    part/pageviewannotator.cpp
    part/pageviewmouseannotation.cpp
    part/pageview.cpp
    part/outsidepdfcomments.cpp
    part/readaloudbar.cpp
    part/reviewfeedbackeditor.cpp
    part/reviewfeedbackoverlay.cpp
    part/documentfeedbackoverlay.cpp
    part/readingoverlaylayer.cpp
    part/readingoverlaylist.cpp
    part/htmldocumentview.cpp
    # Its sources are per platform (htmllivewebview_mac.mm, _win.cpp, _stub.cpp): listed for AUTOMOC.
    part/htmllivewebview.h
    part/htmlreader.cpp
    part/htmlattachmentreader.cpp
    part/parthtml.cpp
    part/partattachment.cpp
    part/snapshotdrawingdialog.cpp
    part/html-viewer/html-viewer.qrc
    part/html-live/html-live.qrc
    part/magnifierview.cpp
    part/pageviewutils.cpp
    part/pdflinkoverridedialog.cpp
    part/presentationsearchbar.cpp
    part/presentationwidget.cpp
    part/propertiesdialog.cpp
    part/revisionviewer.cpp
    part/searchlineedit.cpp
    part/searchwidget.cpp
    part/sidebar.cpp
    part/side_reviews.cpp
    part/snapshottaker.cpp
    part/thumbnaillist.cpp
    part/toc.cpp
    part/toggleactionmenu.cpp
    part/videowidget.cpp
    part/layers.cpp
    part/signaturepartutils.cpp
    part/signaturepartutilskeydelegate.cpp
    part/signaturepartutilsrecentimagesmodel.cpp
    part/signaturepartutilsimageitemdelegate.cpp
    part/signaturepropertiesdialog.cpp
    part/signaturepanel.cpp
    part/signingcertificatelistmodel.cpp
    part/part.qrc
  )
  ki18n_wrap_ui(okularpart_SRCS part/selectcertificatedialog.ui)

if (Qt6TextToSpeech_FOUND)
   set(okularpart_SRCS ${okularpart_SRCS}
      part/tts.cpp)
endif()

kconfig_add_kcfg_files(okularpart_SRCS GENERATE_MOC conf/settings.kcfgc)

add_library(okularpart SHARED ${okularpart_SRCS})
generate_export_header(okularpart BASE_NAME okularpart)

target_link_libraries(okularpart okularcore okulardocscommentsync thoughtsclient thoughtsdesktopsync thoughtsreadaloud thoughtshtmlview thoughtsclaudecode
    ${MATH_LIB}
    Qt6::Svg
    KF6::Archive
    KF6::Bookmarks
    KF6::I18n
    KF6::IconThemes
    KF6::KIOCore
    KF6::KIOWidgets
    KF6::Parts
    KF6::WindowSystem
    KF6::TextWidgets
    KF6::ColorScheme
)
if(TARGET zstd::libzstd)
    target_link_libraries(okularpart zstd::libzstd)
    target_compile_definitions(okularpart PRIVATE THOUGHTS_HAVE_ZSTD=1)
endif()
if(THOUGHTS_HTML_VIEW)
    target_link_libraries(okularpart Qt6::WebView)
    target_compile_definitions(okularpart PRIVATE THOUGHTS_HAVE_QTWEBVIEW=1)
else()
    target_compile_definitions(okularpart PRIVATE THOUGHTS_HAVE_QTWEBVIEW=0)
endif()
# Live HTML pages: a web view of their own (part/htmllivewebview.h), apart from Qt WebView's.
if(THOUGHTS_HTML_VIEW AND APPLE)
    enable_language(OBJCXX)
    # A WKWebView.
    target_sources(okularpart PRIVATE part/htmlmacnative.mm part/htmllivewebview_mac.mm)
    target_link_libraries(okularpart "-framework WebKit" "-framework AppKit")
elseif(WIN32 AND THOUGHTS_WEBVIEW2_SDK)
    # A WebView2, with Microsoft's loader linked in (no WebView2Loader.dll to ship). Embedded
    # pages and HTML editions get one too (part/htmlpagewebview_win.cpp, QWebView's interface for
    # HtmlDocumentView), since Qt WebView has no backend here.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
        set(_webview2_arch arm64)
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(_webview2_arch x86)
    else()
        set(_webview2_arch x64)
    endif()
    set(_webview2_loader "${THOUGHTS_WEBVIEW2_SDK}/${_webview2_arch}/WebView2LoaderStatic.lib")
    if(NOT EXISTS "${THOUGHTS_WEBVIEW2_SDK}/include/WebView2.h" OR NOT EXISTS "${_webview2_loader}")
        message(FATAL_ERROR "THOUGHTS_WEBVIEW2_SDK (${THOUGHTS_WEBVIEW2_SDK}) has no include/WebView2.h or ${_webview2_arch}/WebView2LoaderStatic.lib")
    endif()
    target_sources(okularpart PRIVATE part/webview2host_win.cpp part/webview2host_win.h part/htmllivewebview_win.cpp
        part/htmlpagewebview_win.cpp part/htmlpagewebview_win.h)
    target_compile_definitions(okularpart PRIVATE THOUGHTS_HAVE_WEBVIEW2=1)
    target_include_directories(okularpart SYSTEM PRIVATE "${THOUGHTS_WEBVIEW2_SDK}/include")
    target_link_libraries(okularpart "${_webview2_loader}" advapi32 ole32 user32)
else()
    target_sources(okularpart PRIVATE part/htmllivewebview_stub.cpp)
endif()
if(Poppler_Qt6_FOUND)
    target_link_libraries(okularpart Poppler::Qt6)
    target_compile_definitions(okularpart PRIVATE THOUGHTS_HAVE_POPPLER_QT6=1)
endif()
if (TARGET Qt6::DBus)
    target_link_libraries(okularpart Qt6::DBus)
endif()
if (Phonon4Qt6_FOUND)
   target_link_libraries(okularpart Phonon::phonon4qt6)
endif()

if(KF6Wallet_FOUND)
    target_link_libraries(okularpart KF6::Wallet)
endif()

if (KF6Purpose_FOUND)
    target_link_libraries(okularpart KF6::PurposeWidgets)
endif()

set_target_properties(okularpart PROPERTIES PREFIX "")
set_target_properties(okularpart PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/kf6/parts)

if (Qt6TextToSpeech_FOUND)
   target_link_libraries(okularpart Qt6::TextToSpeech)
endif()

#instead install the part to this path so it can be found
install(TARGETS okularpart DESTINATION ${KDE_INSTALL_PLUGINDIR}/kf6/parts/ )

endif()# End of BUILD_DESKTOP if


########### install files ###############

install(FILES okular.categories  DESTINATION  ${KDE_INSTALL_LOGGINGCATEGORIESDIR})

ki18n_install(po)
if(KF6DocTools_FOUND)
    kdoctools_install(po)
endif()

########### cmake files #################

set(CMAKECONFIG_INSTALL_DIR "${KDE_INSTALL_CMAKEPACKAGEDIR}/Okular6")
configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/Okular6Config.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/Okular6Config.cmake"
  INSTALL_DESTINATION  ${CMAKECONFIG_INSTALL_DIR}
)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/Okular6Config.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/Okular6ConfigVersion.cmake"
  DESTINATION "${CMAKECONFIG_INSTALL_DIR}"
  COMPONENT Devel
)

install(EXPORT Okular6Targets DESTINATION "${CMAKECONFIG_INSTALL_DIR}" FILE Okular6Targets.cmake NAMESPACE Okular::)

install(FILES
   core/stamps.svg
   DESTINATION ${KDE_INSTALL_DATADIR}/okular/pics)

########### summary #################

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)

message("-- Building Desktop User Interface: ${BUILD_DESKTOP}")
message("-- Building Mobile Interface: ${BUILD_MOBILE}")
message("")
