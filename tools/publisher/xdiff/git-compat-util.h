/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The few parts of git's git-compat-util.h that git's xdiff/ (v2.56.0) uses,
 * so that the unmodified xdiff sources build outside git. The definitions
 * follow git v2.56.0 (git-compat-util.h, sane-ctype.h, wrapper.c, usage.c);
 * only what happens on failure differs: see thoughts-xdiff.c.
 */
#ifndef THOUGHTS_XDIFF_GIT_COMPAT_UTIL_H
#define THOUGHTS_XDIFF_GIT_COMPAT_UTIL_H

#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define UNUSED __attribute__((unused))
#define MAYBE_UNUSED __attribute__((__unused__))
#define THOUGHTS_XDIFF_NORETURN __attribute__((__noreturn__))
#define THOUGHTS_XDIFF_PRINTF(f, a) __attribute__((format(printf, f, a)))
#elif defined(_MSC_VER)
#define UNUSED
#define MAYBE_UNUSED
#define THOUGHTS_XDIFF_NORETURN __declspec(noreturn)
#define THOUGHTS_XDIFF_PRINTF(f, a)
#else
#define UNUSED
#define MAYBE_UNUSED
#define THOUGHTS_XDIFF_NORETURN
#define THOUGHTS_XDIFF_PRINTF(f, a)
#endif

#define bitsizeof(x) (CHAR_BIT * sizeof(x))
#define maximum_signed_value_of_type(a) (INTMAX_MAX >> (bitsizeof(intmax_t) - bitsizeof(a)))
#define signed_add_overflows(a, b) ((b) > maximum_signed_value_of_type(a) - (a))

/*
 * git's sane ctype: no locale, ASCII only. xmerge.c's alnum test decides how
 * XDL_MERGE_ZEALOUS_ALNUM coalesces conflicts, so a locale-dependent isalnum()
 * (macOS counts Latin-1 bytes as letters) would change merge results.
 */
#undef isspace
#undef isdigit
#undef isalpha
#undef isalnum
extern const unsigned char thoughts_xdiff_sane_ctype[256];
#define GIT_SPACE 0x01
#define GIT_DIGIT 0x02
#define GIT_ALPHA 0x04
#define sane_istest(x, mask) ((thoughts_xdiff_sane_ctype[(unsigned char)(x)] & (mask)) != 0)
#define isspace(x) sane_istest(x, GIT_SPACE)
#define isdigit(x) sane_istest(x, GIT_DIGIT)
#define isalpha(x) sane_istest(x, GIT_ALPHA)
#define isalnum(x) sane_istest(x, GIT_ALPHA | GIT_DIGIT)

/* git dies here; thoughts-xdiff.c abandons the merge in progress instead. */
THOUGHTS_XDIFF_NORETURN void thoughts_xdiff_bug(const char *file, int line, const char *format, ...) THOUGHTS_XDIFF_PRINTF(3, 4);
#define BUG(...) thoughts_xdiff_bug(__FILE__, __LINE__, __VA_ARGS__)

void *xmalloc(size_t size);
void *xcalloc(size_t nmemb, size_t size);
void *xrealloc(void *ptr, size_t size);

/* xdiff.h names regex_t for diff -I<regex>, which merges never use. */
typedef struct thoughts_xdiff_regex regex_t;
typedef struct {
    ptrdiff_t rm_so;
    ptrdiff_t rm_eo;
} regmatch_t;
int regexec_buf(const regex_t *preg, const char *buf, size_t size, size_t nmatch, regmatch_t pmatch[], int eflags);

#ifdef __cplusplus
}
#endif

#endif
