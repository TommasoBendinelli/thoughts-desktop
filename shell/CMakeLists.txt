
include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_BINARY_DIR}/../
)

# okular

set(okular_SRCS
   activeparttoolbarcontroller.cpp
   agentnotes.cpp
   agentnoteswindow.cpp
   agentnotecomposer.cpp
   agentnotedrawing.cpp
   main.cpp
   developerlog.cpp
   developerlogdialog.cpp
   okular_main.cpp
   buildterminallauncher.cpp
   commentcodexlauncher.cpp
   commentsoverview.cpp
   librarysearch.cpp
   websiteimportdialog.cpp
   documentstyleinformation.cpp
   documentsyncpresentation.cpp
   reviewoverview.cpp
   reviewfeedbackpanel.cpp
   reviewstore.cpp
   reviewtagplan.cpp
   reviewdocumenttools.cpp
   reviewcardeditor.cpp
   reviewcardmerge.cpp
   shell.cpp
   shellutils.cpp
   syncjournal.cpp
   thoughtscommandprotocol.cpp
   todocommands.cpp
   updatecheck.cpp
   welcomescreen.cpp

   ../gui/recentitemsmodel.cpp

   shell.qrc
)

ki18n_wrap_ui(okular_SRCS
    welcomescreen.ui)

file(GLOB ICONS_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/../icons/*-apps-thoughts.png")
if(WIN32)
    # The executable's icon comes from a prebuilt .ico (ecm_add_app_icon needs
    # icotool on Windows); windows use the PNGs, as the Breeze theme only has
    # Okular's icon.
    set(thoughts_ico "${CMAKE_CURRENT_SOURCE_DIR}/../icons/thoughts.ico")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/thoughts-icon.rc" "IDI_ICON1 ICON \"${thoughts_ico}\"\n")
    set(thoughts_icon_qrc "<RCC><qresource prefix=\"/thoughts/icons\">")
    foreach(icon IN LISTS ICONS_SRCS)
        get_filename_component(icon_name "${icon}" NAME)
        string(APPEND thoughts_icon_qrc "<file alias=\"${icon_name}\">${icon}</file>")
    endforeach()
    string(APPEND thoughts_icon_qrc "</qresource></RCC>")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/thoughts-icons.qrc" "${thoughts_icon_qrc}")
    list(APPEND okular_SRCS "${CMAKE_CURRENT_BINARY_DIR}/thoughts-icon.rc" "${CMAKE_CURRENT_BINARY_DIR}/thoughts-icons.qrc")
else()
    ecm_add_app_icon(okular_SRCS ICONS ${ICONS_SRCS} OUTFILE_BASENAME thoughts)
endif()

# Bundle the small VS Code companion so source-range opening works after install.
set(source_selection_root "${CMAKE_CURRENT_SOURCE_DIR}/../tools/vscode-source-selection")
file(GLOB_RECURSE source_selection_files "${source_selection_root}/extension/*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${source_selection_files} "${source_selection_root}/extension.vsixmanifest" "${source_selection_root}/[Content_Types].xml")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${CMAKE_CURRENT_BINARY_DIR}/source-selection.vsix" --format=zip --
    "[Content_Types].xml" extension.vsixmanifest extension
    WORKING_DIRECTORY "${source_selection_root}" COMMAND_ERROR_IS_FATAL ANY)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/source-selection.qrc"
    "<RCC><qresource prefix=\"/thoughts\"><file alias=\"source-selection.vsix\">source-selection.vsix</file></qresource></RCC>")
list(APPEND okular_SRCS "${CMAKE_CURRENT_BINARY_DIR}/source-selection.qrc")

# Review › Mental Model › Refresh runs the generator and its prompt from the app.
set(mental_model_root "${CMAKE_CURRENT_SOURCE_DIR}/../tools/mental-model")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/mental-model.qrc"
    "<RCC><qresource prefix=\"/thoughts/mental-model\"><file alias=\"build.py\">${mental_model_root}/build.py</file>"
    "<file alias=\"prompt.md\">${mental_model_root}/prompt.md</file></qresource></RCC>")
list(APPEND okular_SRCS "${CMAKE_CURRENT_BINARY_DIR}/mental-model.qrc")

if(APPLE)
    enable_language(OBJCXX)
    list(APPEND okular_SRCS macquitevent.mm readerchromemac.mm agentnotesnotifiermac.mm)
else()
    list(APPEND okular_SRCS readerchrome.cpp)
endif()

add_executable(thoughts ${okular_SRCS})

target_link_libraries(thoughts okularcore okulardocscommentsync thoughtsclient thoughtsdesktopsync thoughtsgitclone OpenSSL::Crypto Poppler::Qt6 Qt6::Concurrent Qt6::Network Qt6::Sql KF6::I18n KF6::Parts KF6::WindowSystem KF6::Crash KF6::IconThemes KF6::KIOGui)
if (TARGET Qt6::DBus)
    target_link_libraries(thoughts Qt6::DBus)
endif()
if(APPLE)
    target_link_libraries(thoughts "-framework UserNotifications")
endif()
if (HAVE_X11)
    target_link_libraries(thoughts Qt6::GuiPrivate)
endif()

# Release builds (tools/windows/cross, tools/macos/release.py) name their version and
# the GitHub repository whose releases bring updates; Help › Check for Updates exists
# only then.
set(THOUGHTS_RELEASE_VERSION "" CACHE STRING "Version of a published Thoughts release build")
set(THOUGHTS_UPDATE_REPOSITORY "" CACHE STRING "GitHub owner/repository whose windows-* (or, on macOS, mac-*) releases update this build")
set_property(SOURCE updatecheck.cpp APPEND PROPERTY COMPILE_DEFINITIONS
    THOUGHTS_RELEASE_VERSION="${THOUGHTS_RELEASE_VERSION}" THOUGHTS_UPDATE_REPOSITORY="${THOUGHTS_UPDATE_REPOSITORY}")

install(TARGETS thoughts ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})

add_executable(thoughtsctl
    ../tools/thoughtsctl.cpp
    thoughtscommandprotocol.cpp
    proposalauthoring.cpp
    # Proposal link checks use the source publisher's Project resolution.
    proposallinks.cpp
    # search-review-cards and knowledge-profile read cards as the app does.
    reviewknowledge.cpp
    reviewstore.cpp
    reviewtagplan.cpp
    reviewdocumenttools.cpp
)
set_target_properties(thoughtsctl PROPERTIES MACOSX_BUNDLE FALSE)
# A console program on Windows, so shells wait for it and see its output.
ecm_mark_nongui_executable(thoughtsctl)
target_link_libraries(thoughtsctl Qt6::Core Qt6::Network Qt6::Sql Qt6::Gui Poppler::Qt6 thoughtsdesktopsync KF6::Archive okulardocscommentsync thoughtspublisher)
install(TARGETS thoughtsctl ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})

if(APPLE)
    target_link_libraries(thoughts "-framework AppKit")
    set_target_properties(thoughts PROPERTIES
        MACOSX_BUNDLE_DISPLAY_NAME "Thoughts"
        MACOSX_BUNDLE_BUNDLE_NAME "Thoughts"
        MACOSX_BUNDLE_ICON_FILE "thoughts"
        MACOSX_BUNDLE_LONG_VERSION_STRING "Thoughts ${RELEASE_SERVICE_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${RELEASE_SERVICE_VERSION_MAJOR}.${RELEASE_SERVICE_VERSION_MINOR}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${RELEASE_SERVICE_VERSION}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/MacOSXBundleInfo.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.tbend.thoughts"
        MACOSX_BUNDLE_COPYRIGHT "© 2026 Tommaso Bendinelli. Built on Okular, © 2002-2026 The Okular Developers")
    if(TARGET thoughts-comment-sync)
        add_dependencies(thoughts thoughts-comment-sync)
        add_custom_command(TARGET thoughts POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:thoughts-comment-sync>
                $<TARGET_FILE_DIR:thoughts>/thoughts-comment-sync
            VERBATIM
        )
    endif()
    add_dependencies(thoughts thoughtsctl)
    add_custom_command(TARGET thoughts POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:thoughtsctl>
            $<TARGET_FILE_DIR:thoughts>/thoughtsctl
        VERBATIM
    )
    # The native source publisher and build viewer run from inside the
    # bundle, so Sync needs no Python installation.
    foreach(tool thoughts-publish-sources thoughts-watch-build)
        add_dependencies(thoughts ${tool})
        add_custom_command(TARGET thoughts POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:${tool}>
                $<TARGET_FILE_DIR:thoughts>/${tool}
            VERBATIM
        )
    endforeach()
    # Updates (updatecheck.cpp) install a downloaded Thoughts.dmg with this script,
    # run from a temporary copy once Thoughts has quit.
    target_sources(thoughts PRIVATE ../tools/macos/install-update.sh)
    set_source_files_properties(../tools/macos/install-update.sh PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    # Keep macOS privacy permissions associated with the same app across builds:
    # sign with the persistent local identity of tools/macos/setup-signing.py.
    # On by default where that identity exists; signing then fails rather than
    # falling back to ad hoc signing (tools/macos/README.md).
    if(EXISTS "$ENV{HOME}/Library/Application Support/Thoughts/signing/identity.txt")
        set(_thoughts_signing_identity_found ON)
    else()
        set(_thoughts_signing_identity_found OFF)
    endif()
    option(THOUGHTS_MACOS_PERSISTENT_SIGNING "Sign the app bundle with the persistent local identity on build and install" ${_thoughts_signing_identity_found})
endif()
if(APPLE AND THOUGHTS_MACOS_PERSISTENT_SIGNING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(THOUGHTS_SIGN_APP "${CMAKE_CURRENT_SOURCE_DIR}/../tools/macos/sign-app.py")
    add_custom_command(TARGET thoughts POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${THOUGHTS_SIGN_APP}"
            "$<TARGET_BUNDLE_DIR:thoughts>"
        VERBATIM
    )
    # Install-time processing can invalidate the build signature. Sign last,
    # including when using shell/cmake_install.cmake with LOCAL_ONLY enabled.
    if(IS_ABSOLUTE "${KDE_INSTALL_BUNDLEDIR}")
        set(THOUGHTS_INSTALLED_APP "${KDE_INSTALL_BUNDLEDIR}/thoughts.app")
    else()
        set(THOUGHTS_INSTALLED_APP "\${CMAKE_INSTALL_PREFIX}/${KDE_INSTALL_BUNDLEDIR}/thoughts.app")
    endif()
    # A proposal MCP update only needs the QtCore/Network command helper. Keep
    # the currently installed GUI and its matching libraries untouched.
    install(CODE "
        if(CMAKE_INSTALL_COMPONENT STREQUAL \"thoughts-agent-cli\")
            if(NOT DEFINED THOUGHTS_AGENT_CLI_BUNDLE)
                message(FATAL_ERROR \"Set THOUGHTS_AGENT_CLI_BUNDLE to the stopped installed app.\")
            endif()
            execute_process(COMMAND \"${Python3_EXECUTABLE}\"
                \"${CMAKE_CURRENT_SOURCE_DIR}/../tools/macos/check-stopped-app.py\"
                \"\${THOUGHTS_AGENT_CLI_BUNDLE}\" RESULT_VARIABLE preflight)
            if(NOT preflight EQUAL 0)
                message(FATAL_ERROR \"The Thoughts bundle must be valid and stopped before replacing its helper.\")
            endif()
            file(INSTALL DESTINATION \"\${THOUGHTS_AGENT_CLI_BUNDLE}/Contents/MacOS\"
                TYPE PROGRAM FILES \"$<TARGET_FILE:thoughtsctl>\")
            execute_process(COMMAND \"${Python3_EXECUTABLE}\" \"${THOUGHTS_SIGN_APP}\"
                \"\${THOUGHTS_AGENT_CLI_BUNDLE}\" RESULT_VARIABLE cli_sign_result)
            if(NOT cli_sign_result EQUAL 0)
                message(FATAL_ERROR \"Persistent helper signing failed.\")
            endif()
        endif()
    " COMPONENT thoughts-agent-cli)
    install(CODE "
        execute_process(
            COMMAND \"${Python3_EXECUTABLE}\" \"${THOUGHTS_SIGN_APP}\"
                \"\$ENV{DESTDIR}${THOUGHTS_INSTALLED_APP}\"
            RESULT_VARIABLE thoughts_sign_result)
        if(NOT thoughts_sign_result EQUAL 0)
            message(FATAL_ERROR \"Persistent Thoughts signing failed; do not deploy this bundle.\")
        endif()
    " COMPONENT okular)
endif()

# okular shell data files

install( PROGRAMS org.kde.okular.desktop  DESTINATION  ${KDE_INSTALL_APPDIR} )
install( FILES org.kde.okular.appdata.xml DESTINATION  ${KDE_INSTALL_METAINFODIR} )
