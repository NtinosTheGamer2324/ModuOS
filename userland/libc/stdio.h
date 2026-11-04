#ifndef _STDIO_H
#define _STDIO_H

#include <stdarg.h>
#include <stddef.h>

typedef struct _FILE FILE;
typedef long fpos_t;

/* Function-backed for the same reason as errno (no exported data). */
FILE *__libc_stdin(void);
FILE *__libc_stdout(void);
FILE *__libc_stderr(void);
#define stdin  (__libc_stdin())
#define stdout (__libc_stdout())
#define stderr (__libc_stderr())

#define EOF         (-1)
#define BUFSIZ      1024
#define FOPEN_MAX   16
#define FILENAME_MAX 256
#define L_tmpnam    16
#define TMP_MAX     99999

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

/* Files */
FILE *fopen(const char *path, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *f);
int   fclose(FILE *f);
int   fflush(FILE *f);
void  setbuf(FILE *f, char *buf);
int   setvbuf(FILE *f, char *buf, int mode, size_t size);
int   remove(const char *path);
int   rename(const char *from, const char *to);   /* ENOSYS on ModuOS for now */
FILE *tmpfile(void);
char *tmpnam(char *s);

/* Formatted output */
int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int sprintf(char *s, const char *fmt, ...);
int snprintf(char *s, size_t n, const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vsprintf(char *s, const char *fmt, va_list ap);
int vsnprintf(char *s, size_t n, const char *fmt, va_list ap);

/* Formatted input */
int scanf(const char *fmt, ...);
int fscanf(FILE *f, const char *fmt, ...);
int sscanf(const char *s, const char *fmt, ...);
int vscanf(const char *fmt, va_list ap);
int vfscanf(FILE *f, const char *fmt, va_list ap);
int vsscanf(const char *s, const char *fmt, va_list ap);

/* Character / line I/O */
int   fgetc(FILE *f);
char *fgets(char *s, int n, FILE *f);
int   fputc(int c, FILE *f);
int   fputs(const char *s, FILE *f);
int   getc(FILE *f);
int   getchar(void);
int   putc(int c, FILE *f);
int   putchar(int c);
int   puts(const char *s);
int   ungetc(int c, FILE *f);

/* Binary I/O */
size_t fread(void *ptr, size_t size, size_t n, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t n, FILE *f);

/* Positioning */
int  fgetpos(FILE *f, fpos_t *pos);
int  fseek(FILE *f, long off, int whence);
int  fsetpos(FILE *f, const fpos_t *pos);
long ftell(FILE *f);
void rewind(FILE *f);

/* Errors */
void clearerr(FILE *f);
int  feof(FILE *f);
int  ferror(FILE *f);
void perror(const char *s);

/* POSIX */
int fileno(FILE *f);

#endif /* _STDIO_H */
