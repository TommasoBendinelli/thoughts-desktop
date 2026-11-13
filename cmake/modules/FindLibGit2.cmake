# SPDX-License-Identifier: BSD-3-Clause
# Finds libgit2 (GPL-2.0 with the linking exception), which Import sources ›
# From Git repository clones with. Uses pkg-config when present, else the
# headers and library on CMAKE_PREFIX_PATH (KDE Craft on macOS and Windows).
#
#  LibGit2::LibGit2  imported target
#  LibGit2_VERSION   e.g. 1.7.1

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_LIBGIT2 QUIET libgit2)
endif()

find_path(LibGit2_INCLUDE_DIR NAMES git2.h HINTS ${PC_LIBGIT2_INCLUDE_DIRS})
find_library(LibGit2_LIBRARY NAMES git2 libgit2 HINTS ${PC_LIBGIT2_LIBRARY_DIRS})

if(LibGit2_INCLUDE_DIR AND EXISTS "${LibGit2_INCLUDE_DIR}/git2/version.h")
    file(STRINGS "${LibGit2_INCLUDE_DIR}/git2/version.h" _libgit2_version REGEX "^#define LIBGIT2_VERSION +\"[^\"]+\"")
    string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" LibGit2_VERSION "${_libgit2_version}")
    unset(_libgit2_version)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LibGit2
    REQUIRED_VARS LibGit2_LIBRARY LibGit2_INCLUDE_DIR
    VERSION_VAR LibGit2_VERSION)

if(LibGit2_FOUND AND NOT TARGET LibGit2::LibGit2)
    add_library(LibGit2::LibGit2 UNKNOWN IMPORTED)
    set_target_properties(LibGit2::LibGit2 PROPERTIES
        IMPORTED_LOCATION "${LibGit2_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LibGit2_INCLUDE_DIR}")
endif()

mark_as_advanced(LibGit2_INCLUDE_DIR LibGit2_LIBRARY)

include(FeatureSummary)
set_package_properties(LibGit2 PROPERTIES
    URL "https://libgit2.org/"
    DESCRIPTION "Git library; Thoughts clones repositories for Import sources with it")
