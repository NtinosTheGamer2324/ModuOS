#ifndef _STRING_H
#define _STRING_H

#include <stddef.h>

/* ---- ISO C ---- */
void  *memcpy(void *__restrict dest, const void *__restrict src, size_t n);
void  *memmove(void *dest, const void *src, size_t n);
void  *memset(void *dest, int val, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memchr(const void *s, int c, size_t n);

char  *strcpy(char *__restrict dest, const char *__restrict src);
char  *strncpy(char *__restrict dest, const char *__restrict src, size_t n);
char  *strcat(char *__restrict dest, const char *__restrict src);
char  *strncat(char *__restrict dest, const char *__restrict src, size_t n);

int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strcoll(const char *a, const char *b);
size_t strxfrm(char *__restrict dest, const char *__restrict src, size_t n);

char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char  *strpbrk(const char *s, const char *accept);
char  *strstr(const char *haystack, const char *needle);
char  *strtok(char *__restrict s, const char *__restrict delim);

char  *strerror(int errnum);
size_t strlen(const char *s);

/* ---- Common extensions (not ISO) ---- */
size_t strnlen(const char *s, size_t maxlen);
char  *strdup(const char *s);
char  *strndup(const char *s, size_t n);
int    strcasecmp(const char *a, const char *b);
int    strncasecmp(const char *a, const char *b, size_t n);
size_t strlcpy(char *dest, const char *src, size_t size);
size_t strlcat(char *dest, const char *src, size_t size);
size_t safe_strcpy(char *dest, size_t dest_sz, const char *src);
size_t safe_strcat(char *dest, size_t dest_sz, const char *src);

#endif /* _STRING_H */
