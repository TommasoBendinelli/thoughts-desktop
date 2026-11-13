/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Runs git's xdl_merge() as builtin/merge-file.c of git v2.56.0 does, and
 * supplies the helpers git-compat-util.h declares.
 */
#include "thoughts-xdiff.h"

#include "xinclude.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>

#if defined(_MSC_VER) && !defined(__clang__)
#define THOUGHTS_XDIFF_THREAD_LOCAL __declspec(thread)
#else
#define THOUGHTS_XDIFF_THREAD_LOCAL _Thread_local
#endif

/* git ctype.c: sane_ctype restricted to GIT_SPACE, GIT_DIGIT and GIT_ALPHA. */
enum { S = GIT_SPACE, A = GIT_ALPHA, D = GIT_DIGIT };
const unsigned char thoughts_xdiff_sane_ctype[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, S, S, 0, 0, S, 0, 0, /*   0.. 15 */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /*  16.. 31 */
    S, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /*  32.. 47 */
    D, D, D, D, D, D, D, D, D, D, 0, 0, 0, 0, 0, 0, /*  48.. 63 */
    0, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, /*  64.. 79 */
    A, A, A, A, A, A, A, A, A, A, A, 0, 0, 0, 0, 0, /*  80.. 95 */
    0, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, /*  96..111 */
    A, A, A, A, A, A, A, A, A, A, A, 0, 0, 0, 0, 0, /* 112..127 */
    /* Nothing in the 128.. range */
};

/*
 * git dies on BUG() and on exhausted memory. Here they abandon the merge in
 * progress: a longjmp() back to thoughts_xdiff_merge_file(), which crosses
 * only xdiff's C frames. What xdiff allocated so far leaks.
 */
static THOUGHTS_XDIFF_THREAD_LOCAL jmp_buf *abandon;
static THOUGHTS_XDIFF_THREAD_LOCAL char failure[256];

static THOUGHTS_XDIFF_NORETURN void abandon_merge(void)
{
    if (abandon) {
        longjmp(*abandon, 1);
    }
    fprintf(stderr, "%s\n", failure);
    abort();
}

void thoughts_xdiff_bug(const char *file, int line, const char *format, ...)
{
    va_list arguments;
    int used = snprintf(failure, sizeof(failure), "BUG: %s:%d: ", file, line);
    if (used < 0 || (size_t)used >= sizeof(failure)) {
        used = 0;
    }
    va_start(arguments, format);
    vsnprintf(failure + used, sizeof(failure) - (size_t)used, format, arguments);
    va_end(arguments);
    abandon_merge();
}

static THOUGHTS_XDIFF_NORETURN void out_of_memory(size_t size)
{
    /* git: die("Out of memory, malloc failed ...") */
    snprintf(failure, sizeof(failure), "fatal: Out of memory, malloc failed (tried to allocate %llu bytes)", (unsigned long long)size);
    abandon_merge();
}

/* git wrapper.c: a zero-size request still returns memory. */
void *xmalloc(size_t size)
{
    void *memory = malloc(size);
    if (!memory && !size) {
        memory = malloc(1);
    }
    if (!memory) {
        out_of_memory(size);
    }
    return memory;
}

void *xcalloc(size_t nmemb, size_t size)
{
    void *memory = calloc(nmemb, size);
    if (!memory && (!nmemb || !size)) {
        memory = calloc(1, 1);
    }
    if (!memory) {
        out_of_memory(nmemb * size);
    }
    return memory;
}

void *xrealloc(void *ptr, size_t size)
{
    void *memory;
    if (!size) {
        free(ptr);
        return xmalloc(0);
    }
    memory = realloc(ptr, size);
    if (!memory) {
        out_of_memory(size);
    }
    return memory;
}

int regexec_buf(const regex_t *preg, const char *buf, size_t size, size_t nmatch, regmatch_t pmatch[], int eflags)
{
    (void)preg;
    (void)buf;
    (void)size;
    (void)nmatch;
    (void)pmatch;
    (void)eflags;
    BUG("diff -I<regex> is not supported");
}

/* xdiff-interface.h / xdiff-interface.c of git v2.56.0 */
#define MAX_XDIFF_SIZE (1024UL * 1024 * 1023)
#define FIRST_FEW_BYTES 8000
static int buffer_is_binary(const char *ptr, unsigned long size)
{
    if (FIRST_FEW_BYTES < size) {
        size = FIRST_FEW_BYTES;
    }
    return size && !!memchr(ptr, 0, size);
}

int thoughts_xdiff_merge_file(const thoughts_xdiff_side *current, const thoughts_xdiff_side *base, const thoughts_xdiff_side *incoming,
                              char **result, long *result_size, int *failed_side, const char **message)
{
    const thoughts_xdiff_side *sides[3] = {current, base, incoming};
    mmfile_t files[3];
    xmparam_t xmp;
    mmbuffer_t merged = {0, 0};
    jmp_buf target;
    jmp_buf *volatile outer = abandon;
    int status;
    int i;

    *result = NULL;
    *result_size = 0;
    *failed_side = -1;
    *message = NULL;
    /* merge-file reads the files in order and stops at the first one it refuses. */
    for (i = 0; i < 3; i++) {
        if (sides[i]->size < 0 || (unsigned long)sides[i]->size > MAX_XDIFF_SIZE
            || buffer_is_binary(sides[i]->data, (unsigned long)sides[i]->size)) {
            *failed_side = i;
            return THOUGHTS_XDIFF_BINARY;
        }
        /* xdiff reads a size-0 file through a non-null pointer, as read_mmfile() leaves it. */
        files[i].ptr = sides[i]->size ? (char *)sides[i]->data : (char *)"";
        files[i].size = sides[i]->size;
    }

    /* cmd_merge_file(): no option but -p and -L, no merge.conflictstyle. */
    memset(&xmp, 0, sizeof(xmp));
    xmp.level = XDL_MERGE_ZEALOUS_ALNUM;
    xmp.style = 0;
    xmp.favor = 0;
    xmp.marker_size = 0; /* xdl_merge() uses DEFAULT_CONFLICT_MARKER_SIZE */
    xmp.xpp.flags = 0; /* the Myers diff, no whitespace or indent options */
    xmp.ancestor = base->label;
    xmp.file1 = current->label;
    xmp.file2 = incoming->label;

    if (setjmp(target)) {
        abandon = outer;
        *message = failure;
        return THOUGHTS_XDIFF_FAILED;
    }
    abandon = &target;
    status = xdl_merge(&files[1], &files[0], &files[2], &xmp, &merged);
    abandon = outer;
    if (status < 0) {
        free(merged.ptr);
        /* merge-file returns -1 without a message */
        *message = "git merge-file failed with exit code 255";
        return THOUGHTS_XDIFF_FAILED;
    }
    *result = merged.ptr;
    *result_size = merged.size;
    return status > 127 ? 127 : status;
}

void thoughts_xdiff_free(char *result)
{
    free(result);
}
