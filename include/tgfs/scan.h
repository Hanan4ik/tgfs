#ifndef TGFS_SCAN_H
#define TGFS_SCAN_H
#include <stdio.h>
void tgfs_scan_help(FILE *stream);
void tgfs_init_help(FILE *stream);
int tgfs_init_main(int argc, char **argv);
int tgfs_scan_main(int argc, char **argv);
#endif
