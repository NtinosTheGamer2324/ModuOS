#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX     32767

typedef struct { int       quot, rem; } div_t;
typedef struct { long      quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;

/* Memory */
void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void  free(void *ptr);

/* Process control */
__attribute__((noreturn)) void abort(void);
__attribute__((noreturn)) void exit(int status);
__attribute__((noreturn)) void _Exit(int status);
int   atexit(void (*fn)(void));
char *getenv(const char *name);
int   system(const char *cmd);

/* Arithmetic */
int       abs(int x);
long      labs(long x);
long long llabs(long long x);
div_t     div(int a, int b);
ldiv_t    ldiv(long a, long b);
lldiv_t   lldiv(long long a, long long b);

/* Conversion */
int                atoi(const char *s);
long               atol(const char *s);
long long          atoll(const char *s);
long               strtol(const char *s, char **end, int base);
long long          strtoll(const char *s, char **end, int base);
unsigned long      strtoul(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);

/* Pseudo-random */
int  rand(void);
void srand(unsigned int seed);

/* Searching / sorting */
void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *));
void  qsort(void *base, size_t n, size_t size,
            int (*cmp)(const void *, const void *));

/* ---- POSIX-ish / ModuOS extensions ---- */
int  putenv(const char *kv);
int  setenv(const char *key, const char *val, int overwrite);   /* POSIX 3-arg form */
int  unsetenv(const char *key);
void itoa(int value, char *str, int base);
void ulltoa(unsigned long long value, char *str, int base, int upper);

#endif /* _STDLIB_H */
