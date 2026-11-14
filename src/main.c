#include "tgfs/tgfs.h"
#include <stdio.h>
#include <string.h>

static int print_entry(const char *name, const char *object_id, void *arg)
{
    (void)object_id;
    return fprintf((FILE *)arg, "%s\n", name) < 0;
}

int main(int argc, char **argv)
{
    if (argc != 5 || strcmp(argv[1], "--snapshot") ||
        (strcmp(argv[3], "ls") && strcmp(argv[3], "cat"))) {
        fprintf(stderr, "usage: tgfs --snapshot DIRECTORY {ls|cat} /absolute/path\n");
        return 2;
    }
    tgfs_error error = {{0}};
    tgfs *fs = NULL;
    if (tgfs_open_snapshot(argv[2], &fs, &error)) {
        fprintf(stderr, "tgfs: %s\n", error.message); return 1;
    }
    int result = !strcmp(argv[3], "ls")
        ? tgfs_list(fs, argv[4], print_entry, stdout, &error)
        : tgfs_cat(fs, argv[4], stdout, &error);
    if (!result && fflush(stdout)) { snprintf(error.message, sizeof(error.message), "cannot flush stdout"); result = -1; }
    if (result) fprintf(stderr, "tgfs: %s\n", error.message);
    tgfs_close(fs);
    return result ? 1 : 0;
}
