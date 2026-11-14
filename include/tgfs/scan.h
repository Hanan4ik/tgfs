#ifndef TGFS_SCAN_H
#define TGFS_SCAN_H
#include <stdio.h>
void tgfs_scan_help(FILE *stream);
void tgfs_init_help(FILE *stream);
int tgfs_init_main(int argc, char **argv);
void tgfs_put_help(FILE *stream);
int tgfs_put_main(int argc, char **argv);
void tgfs_read_help(FILE *stream);
int tgfs_read_main(int argc, char **argv);
int tgfs_scan_main(int argc, char **argv);
#endif
