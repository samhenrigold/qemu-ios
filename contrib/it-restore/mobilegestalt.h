#ifndef IT_RESTORE_MOBILEGESTALT_H
#define IT_RESTORE_MOBILEGESTALT_H
#include <stdio.h>
int it_prepare_mobilegestalt(FILE *log);
int it_verify_mobilegestalt(FILE *log);
/* Reapply an existing plan after native migration, without rebuilding the cache. */
int it_reapply_mobilegestalt(FILE *log, const char *path);
#endif
