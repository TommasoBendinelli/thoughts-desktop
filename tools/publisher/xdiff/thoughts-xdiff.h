/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * `git merge-file -p -L <current> -L <base> -L <incoming> current base incoming`
 * of git v2.56.0, without the files and the process: the same xdl_merge()
 * call with the settings builtin/merge-file.c uses when no git configuration
 * and no other option applies.
 */
#ifndef THOUGHTS_XDIFF_H
#define THOUGHTS_XDIFF_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct thoughts_xdiff_side {
    const char *data;
    long size;
    const char *label; /* -L; git's default is the file name */
} thoughts_xdiff_side;

enum {
    /* A side is "binary" (a NUL in its first 8000 bytes) or over 1023 MiB;
       *failed_side is 0 (current), 1 (base) or 2 (incoming). */
    THOUGHTS_XDIFF_BINARY = -1,
    /* xdl_merge() failed, ran out of memory or hit one of its BUG() checks;
       *message is what git would print to stderr ("fatal: Out of memory, ...",
       "BUG: ...") or the server's message for a silent exit status 255. */
    THOUGHTS_XDIFF_FAILED = -2,
};

/*
 * Returns git merge-file's exit status for a merge it completes (0 when clean,
 * else the conflict count capped at 127) and stores the merged bytes in
 * *result / *result_size; free them with thoughts_xdiff_free(). Returns a
 * negative THOUGHTS_XDIFF_* code when there is no result.
 */
int thoughts_xdiff_merge_file(const thoughts_xdiff_side *current, const thoughts_xdiff_side *base, const thoughts_xdiff_side *incoming,
                              char **result, long *result_size, int *failed_side, const char **message);
void thoughts_xdiff_free(char *result);

#ifdef __cplusplus
}
#endif

#endif
