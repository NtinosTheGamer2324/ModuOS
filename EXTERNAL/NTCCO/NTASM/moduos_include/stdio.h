#pragma once
/* ModuOS stdio stub for NTASM — redirects to ntasm_moduos_compat.c */
#include "../../userland/libc.h"

typedef struct _MFILE FILE;

extern FILE *stdout;
extern FILE *stderr;
extern FILE *stdin;

FILE   *fopen(const char *path, const char *mode);
int     fclose(FILE *f);
size_t  fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
int     fputc(int c, FILE *f);
int     fputs(const char *s, FILE *f);
int     fprintf(FILE *f, const char *fmt, ...);
