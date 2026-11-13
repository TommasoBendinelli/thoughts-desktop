/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

/* Build-time Thoughts settings, from the CMake cache variables of the same
   names (see CMakeLists.txt). */

/* The server address a new installation starts with. */
#define THOUGHTS_DEFAULT_SERVER_URL "@THOUGHTS_DEFAULT_SERVER_URL@"

/* The address earlier versions of this build always used. An installation
   that synchronized before it saved an address keeps it. Empty: none. */
#define THOUGHTS_LEGACY_SERVER_URL "@THOUGHTS_LEGACY_SERVER_URL@"

/* The page File › A/B testing opens in the browser. Empty: no such entry. */
#define THOUGHTS_AB_TESTING_URL "@THOUGHTS_AB_TESTING_URL@"
