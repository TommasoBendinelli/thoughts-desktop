/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test driver for merge_differential.py: the native `git merge-file -p
 * -L FEDORA_CURRENT -L COMMON_BASE -L PHONE_INCOMING current base incoming`.
 * Usage: mergecheck CASES OUT < names
 * For each name on stdin (one per line) reads CASES/<name>/{current,base,incoming}
 * and writes OUT/<name>.out (the merged bytes) and OUT/<name>.code (the exit
 * status git would have, or "error: ..." when git would refuse). Plain C, so
 * that it also builds alone with clang-cl for a run under Wine.
 */
#include "../xdiff/thoughts-xdiff.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path, long *size)
{
    FILE *file = fopen(path, "rb");
    char *data;
    long length;
    if (!file) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(2);
    }
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    fseek(file, 0, SEEK_SET);
    data = malloc(length ? (size_t)length : 1);
    if (!data || (length && fread(data, (size_t)length, 1, file) != 1)) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    fclose(file);
    *size = length;
    return data;
}

static void write_file(const char *path, const char *data, long size)
{
    FILE *file = fopen(path, "wb");
    if (!file || (size && fwrite(data, (size_t)size, 1, file) != 1) || fclose(file)) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(2);
    }
}

int main(int argc, char **argv)
{
    static const char *const names[3] = {"current", "base", "incoming"};
    static const char *const labels[3] = {"FEDORA_CURRENT", "COMMON_BASE", "PHONE_INCOMING"};
    char line[1024];
    char path[4096];
    if (argc != 3) {
        fprintf(stderr, "usage: mergecheck CASES OUT < names\n");
        return 2;
    }
    while (fgets(line, sizeof(line), stdin)) {
        thoughts_xdiff_side sides[3];
        char *result = NULL;
        long result_size = 0;
        int failed_side = -1;
        const char *message = NULL;
        char code[512];
        int status;
        int i;
        line[strcspn(line, "\r\n")] = 0;
        if (!*line) {
            continue;
        }
        for (i = 0; i < 3; i++) {
            snprintf(path, sizeof(path), "%s/%s/%s", argv[1], line, names[i]);
            sides[i].data = read_file(path, &sides[i].size);
            sides[i].label = labels[i];
        }
        status = thoughts_xdiff_merge_file(&sides[0], &sides[1], &sides[2], &result, &result_size, &failed_side, &message);
        if (status >= 0) {
            snprintf(code, sizeof(code), "%d\n", status);
        } else if (status == THOUGHTS_XDIFF_BINARY) {
            snprintf(code, sizeof(code), "error: Cannot merge binary files: %s\n", names[failed_side]);
        } else {
            snprintf(code, sizeof(code), "%s\n", message);
        }
        snprintf(path, sizeof(path), "%s/%s.out", argv[2], line);
        write_file(path, result ? result : "", result ? result_size : 0);
        snprintf(path, sizeof(path), "%s/%s.code", argv[2], line);
        write_file(path, code, (long)strlen(code));
        thoughts_xdiff_free(result);
        for (i = 0; i < 3; i++) {
            free((char *)sides[i].data);
        }
    }
    return 0;
}
