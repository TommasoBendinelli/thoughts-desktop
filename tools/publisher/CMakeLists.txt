# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
# git v2.56.0's xdiff, for the three-way merges Fedora runs with git merge-file
# (xdiff/README.md). C, LGPL-2.1-or-later.
add_library(thoughtsxdiff OBJECT
    xdiff/thoughts-xdiff.c
    xdiff/xdiffi.c
    xdiff/xemit.c
    xdiff/xhistogram.c
    xdiff/xmerge.c
    xdiff/xpatience.c
    xdiff/xprepare.c
    xdiff/xutils.c
)
set_target_properties(thoughtsxdiff PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

# The source publisher and build viewer that Thoughts runs during Sync.
add_library(thoughtspublisher STATIC
    argparse.cpp
    buildinputs.cpp
    cssreferences.cpp
    frozeninputs.cpp
    htmlpages.cpp
    managedsource.cpp
    markdownlinks.cpp
    manifestcommands.cpp
    mediatypes.cpp
    progress.cpp
    projectconfig.cpp
    publishcommand.cpp
    projectdocuments.cpp
    pyfs.cpp
    pyio.cpp
    pyjson.cpp
    pyops.cpp
    pystr.cpp
    proposalclaims.cpp
    pyurl.cpp
    reviewreceipts.cpp
    sha256.cpp
    sourceapi.cpp
    sourceconflictrecord.cpp
    sourceinventory.cpp
    sourcemerge.cpp
    sourcepublisher.cpp
    sourcereconciliation.cpp
    statelock.cpp
    texcommands.cpp
    thoughtignore.cpp
    $<TARGET_OBJECTS:thoughtsxdiff>
)
set_target_properties(thoughtspublisher PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(thoughtspublisher PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(thoughtspublisher PUBLIC Qt6::Core Qt6::Network Qt6::Sql thoughtsplatform thoughtsagentproposals)
target_compile_features(thoughtspublisher PUBLIC cxx_std_20)
# Failures unwind to the command's top level as Python's exceptions do.
kde_target_enable_exceptions(thoughtspublisher PUBLIC)
if(WIN32)
    target_link_libraries(thoughtspublisher PUBLIC bcrypt)
endif()

add_executable(thoughts-publish-sources publishsources.cpp)
set_target_properties(thoughts-publish-sources PROPERTIES MACOSX_BUNDLE FALSE)
ecm_mark_nongui_executable(thoughts-publish-sources)
target_link_libraries(thoughts-publish-sources thoughtspublisher)
if(WIN32)
    target_link_libraries(thoughts-publish-sources shell32)
endif()
install(TARGETS thoughts-publish-sources ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})

add_executable(thoughts-watch-build watchbuild.cpp)
set_target_properties(thoughts-watch-build PROPERTIES MACOSX_BUNDLE FALSE)
ecm_mark_nongui_executable(thoughts-watch-build)
target_link_libraries(thoughts-watch-build thoughtspublisher)
if(WIN32)
    target_link_libraries(thoughts-watch-build shell32)
endif()
install(TARGETS thoughts-watch-build ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})

if(BUILD_TESTING)
    add_executable(pyjsoncheck tests/pyjsoncheck.cpp)
    ecm_mark_nongui_executable(pyjsoncheck)
    target_link_libraries(pyjsoncheck thoughtspublisher)
    # tests/merge_differential.py compares it with git merge-file.
    add_executable(mergecheck tests/mergecheck.c $<TARGET_OBJECTS:thoughtsxdiff>)
    ecm_mark_nongui_executable(mergecheck)
    add_executable(mediatypescheck tests/mediatypescheck.cpp)
    ecm_mark_nongui_executable(mediatypescheck)
    target_link_libraries(mediatypescheck thoughtspublisher)
endif()
