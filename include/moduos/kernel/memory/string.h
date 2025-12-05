#ifndef STRING_H
#define STRING_H

#include <stddef.h>
#include <stdint.h>

/* Memory manipulation functions */
void *memset(void *dest, int val, size_t len);
void *memcpy(void *dest, const void *src, size_t len);
void *memmove(void *dest, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);

/* String length and comparison */
size_t strlen(const char *str);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);

/* String copying */
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);

/* String concatenation */
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t n);

/* String searching */
char *strchr(const char *str, int c);
char *strrchr(const char *str, int c);
char *strstr(const char *haystack, const char *needle);

/* String conversion */
char *itoa(int value, char *str, int base);
int atoi(const char *str);


int snprintf(char *str, size_t size, const char *format, ...);
char* str_append(int count, ...);

#endif