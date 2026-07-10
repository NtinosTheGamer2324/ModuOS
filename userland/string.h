#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* --- 1. SEARCH & COMPARISON --- */

static inline size_t strlen(const char *str) {
    const char *s = str;
    while (*s) ++s;
    return s - str;
}

static inline int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) { ++s1; ++s2; }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

static inline int strncmp(const char *s1, const char *s2, size_t n) {
    if (n == 0) return 0;
    while (n-- > 0 && *s1 && (*s1 == *s2)) {
        if (n == 0) break;
        ++s1; ++s2;
    }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

static inline int strcasecmp(const char *s1, const char *s2) {
    while (*s1) {
        int c1 = (*s1 >= 'A' && *s1 <= 'Z') ? *s1 + 32 : *s1;
        int c2 = (*s2 >= 'A' && *s2 <= 'Z') ? *s2 + 32 : *s2;
        if (c1 != c2) return c1 - c2;
        s1++; s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

static inline char *strchr(const char *str, int c) {
    while (*str) { if (*str == (char)c) return (char *)str; ++str; }
    return (c == '\0') ? (char *)str : NULL;
}

static inline char *strrchr(const char *str, int c) {
    const char *last = NULL;
    while (*str) { if (*str == (char)c) last = str; ++str; }
    return (c == '\0') ? (char *)str : (char *)last;
}

static inline char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && (*h == *n)) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return NULL;
}

/* --- 2. THE TOKENIZER (Crucial for DB parsing) --- */

static inline char *strtok(char *str, const char *delim) {
    static char *last = NULL;
    if (str) last = str;
    if (!last || *last == '\0') return NULL;

    // Skip leading delimiters
    while (*last && strchr(delim, *last)) last++;
    if (*last == '\0') return NULL;

    char *token = last;
    // Find end of token
    while (*last && !strchr(delim, *last)) last++;
    
    if (*last != '\0') {
        *last = '\0';
        last++;
    }
    return token;
}

/* --- 3. SAFE COPY & CONCAT (BSD Style) --- */

static inline size_t strlcpy(char *dest, const char *src, size_t size) {
    size_t i = 0;
    size_t src_len = strlen(src);
    if (size > 0) {
        for (i = 0; i < size - 1 && src[i] != '\0'; i++) dest[i] = src[i];
        dest[i] = '\0';
    }
    return src_len;
}

static inline size_t strlcat(char *dest, const char *src, size_t size) {
    size_t dest_len = strlen(dest);
    size_t src_len = strlen(src);
    if (dest_len >= size) return size + src_len;
    strlcpy(dest + dest_len, src, size - dest_len);
    return dest_len + src_len;
}

static inline char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++) != '\0');
    return dest;
}

static inline char *strncpy(char *dest, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dest[i] = src[i];
    for (; i < n; i++) dest[i] = '\0';
    return dest;
}

static inline char *strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++) != '\0');
    return dest;
}

static inline char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (*d) d++;
    size_t i = 0;
    for (; i < n && src[i]; i++) d[i] = src[i];
    d[i] = '\0';
    return dest;
}

static inline size_t safe_strcpy(char *dest, size_t dest_sz, const char *src) {
    if (!dest || !src || dest_sz == 0) return 0;
    size_t i = 0;
    for (; i + 1 < dest_sz && src[i]; i++) dest[i] = src[i];
    dest[i] = '\0';
    return i;
}

// Your requested implementation
static inline size_t safe_strcat(char *dest, size_t dest_sz, const char *src) {
    return strlcat(dest, src, dest_sz);
}

/* --- 4. MEMORY OPERATIONS --- */

static inline void *memset(void *dest, int val, size_t len) {
    unsigned char *ptr = dest;
    while (len--) *ptr++ = (unsigned char)val;
    return dest;
}

static inline void *memcpy(void *dest, const void *src, size_t len) {
    unsigned char *d = dest; const unsigned char *s = src;
    while (len--) *d++ = *s++;
    return dest;
}

static inline void *memmove(void *dest, const void *src, size_t n) {
    unsigned char *d = dest; const unsigned char *s = src;
    if (d < s) { while (n--) *d++ = *s++; }
    else if (d > s) { d += n; s += n; while (n--) *--d = *--s; }
    return dest;
}

static inline int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = s1, *p2 = s2;
    while (n--) { if (*p1 != *p2) return *p1 - *p2; p1++; p2++; }
    return 0;
}

/* --- 5. NUMERIC CONVERSION --- */

static inline int atoi(const char *str) {
    int res = 0, sign = 1;
    while (*str == ' ' || *str == '\t') str++;
    if (*str == '-') { sign = -1; str++; }
    while (*str >= '0' && *str <= '9') { res = res * 10 + (*str - '0'); str++; }
    return res * sign;
}

// Optimized itoa/ulltoa helpers
static inline void ulltoa(unsigned long long value, char *str, int base, int upper) {
    char *p = str;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (value == 0) { *p++ = '0'; *p = '\0'; return; }
    while (value > 0) { *p++ = digits[value % base]; value /= base; }
    *p = '\0';
    // Reverse
    char *start = str, *end = p - 1;
    while (start < end) { char t = *start; *start++ = *end; *end-- = t; }
}

static inline void itoa(int value, char *str, int base) {
    if (value < 0 && base == 10) { *str++ = '-'; ulltoa((unsigned int)-value, str, 10, 0); }
    else { ulltoa((unsigned int)value, str, base, 0); }
}

/* --- 6. THE PRINTF ENGINE --- */

/*
 * snprintf — bounded format-to-buffer.
 *
 * Supported:
 *   Specifiers : d i u x X o p s c %
 *   Length mods: l  ll  z (size_t)
 *   Flags      : -  0  +  (space)  #
 *   Width      : %10d  %-10s
 *   Precision  : %.4s  %08.4d
 *
 * Always NUL-terminates. Returns total characters that would have been
 * written (C99 semantics), even if truncated.
 */
static inline int snprintf(char *str, size_t size, const char *fmt, ...) {
    if (!str || size == 0) return 0;

    va_list ap;
    va_start(ap, fmt);

    char *out = str;
    size_t rem = size - 1;   /* reserve one byte for NUL */
    int total  = 0;

/* Emit one character — always count it, only store if room left */
#define _EMIT(ch) do { if (rem > 0) { *out++ = (char)(ch); rem--; } total++; } while(0)
#define _EMITS(s, n) do { for (int _i = 0; _i < (int)(n); _i++) _EMIT((s)[_i]); } while(0)

    for (const char *p = fmt; *p; ) {
        if (*p != '%') { _EMIT(*p++); continue; }
        p++;                        /* skip '%' */
        if (!*p) break;

        /* ── Flags ── */
        int flag_left  = 0;
        int flag_zero  = 0;
        int flag_plus  = 0;
        int flag_space = 0;
        int flag_hash  = 0;
        for (;;) {
            if      (*p == '-') { flag_left  = 1; p++; }
            else if (*p == '0') { flag_zero  = 1; p++; }
            else if (*p == '+') { flag_plus  = 1; p++; }
            else if (*p == ' ') { flag_space = 1; p++; }
            else if (*p == '#') { flag_hash  = 1; p++; }
            else break;
        }

        /* ── Width ── */
        int width = 0;
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p++ - '0'); }

        /* ── Precision ── */
        int prec = -1;
        if (*p == '.') {
            p++; prec = 0;
            while (*p >= '0' && *p <= '9') { prec = prec * 10 + (*p++ - '0'); }
        }

        /* ── Length modifier ── */
        int lmod = 0; /* 0=int  1=long  2=long long  3=size_t */
        if (*p == 'l') {
            lmod = 1; p++;
            if (*p == 'l') { lmod = 2; p++; }
        } else if (*p == 'z') {
            lmod = 3; p++;
        }

        char spec = *p++;

        /* ── String / char: handle directly (no numeric pipeline) ── */
        if (spec == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int slen = (int)strlen(s);
            if (prec >= 0 && slen > prec) slen = prec;
            int pad = (width > slen) ? (width - slen) : 0;
            if (!flag_left) for (int i = 0; i < pad; i++) _EMIT(' ');
            _EMITS(s, slen);
            if ( flag_left) for (int i = 0; i < pad; i++) _EMIT(' ');
            continue;
        }
        if (spec == 'c') {
            char c = (char)va_arg(ap, int);
            int pad = (width > 1) ? (width - 1) : 0;
            if (!flag_left) for (int i = 0; i < pad; i++) _EMIT(' ');
            _EMIT(c);
            if ( flag_left) for (int i = 0; i < pad; i++) _EMIT(' ');
            continue;
        }
        if (spec == '%') { _EMIT('%'); continue; }

        /* ── Numeric: convert to string in tmp[], then emit with padding ── */
        char tmp[68];
        int  val_len  = 0;
        char sign_ch  = 0;  /* '-'  '+'  ' '  or 0 */

        switch (spec) {

        case 'd': case 'i': {
            long long n;
            if      (lmod == 2) n = va_arg(ap, long long);
            else if (lmod == 1) n = va_arg(ap, long);
            else if (lmod == 3) n = (long long)va_arg(ap, size_t);
            else                n = va_arg(ap, int);
            if      (n < 0)      { sign_ch = '-'; ulltoa((unsigned long long)-n, tmp, 10, 0); }
            else if (flag_plus)  { sign_ch = '+'; ulltoa((unsigned long long) n, tmp, 10, 0); }
            else if (flag_space) { sign_ch = ' '; ulltoa((unsigned long long) n, tmp, 10, 0); }
            else                 {                ulltoa((unsigned long long) n, tmp, 10, 0); }
            val_len = (int)strlen(tmp);
            break;
        }

        case 'u': {
            unsigned long long n;
            if      (lmod == 2) n = va_arg(ap, unsigned long long);
            else if (lmod == 1) n = va_arg(ap, unsigned long);
            else if (lmod == 3) n = va_arg(ap, size_t);
            else                n = (unsigned long long)va_arg(ap, unsigned int);
            ulltoa(n, tmp, 10, 0);
            val_len = (int)strlen(tmp);
            break;
        }

        case 'o': {
            unsigned long long n;
            if      (lmod == 2) n = va_arg(ap, unsigned long long);
            else if (lmod == 1) n = va_arg(ap, unsigned long);
            else                n = (unsigned long long)va_arg(ap, unsigned int);
            ulltoa(n, tmp, 8, 0);
            val_len = (int)strlen(tmp);
            /* # flag: prepend "0" if not already "0" */
            if (flag_hash && tmp[0] != '0') {
                memmove(tmp + 1, tmp, (size_t)val_len + 1);
                tmp[0] = '0'; val_len++;
            }
            break;
        }

        case 'x': case 'X': {
            unsigned long long n;
            if      (lmod == 2) n = va_arg(ap, unsigned long long);
            else if (lmod == 1) n = va_arg(ap, unsigned long);
            else if (lmod == 3) n = va_arg(ap, size_t);
            else                n = (unsigned long long)va_arg(ap, unsigned int);
            ulltoa(n, tmp, 16, spec == 'X');
            val_len = (int)strlen(tmp);
            /* # flag: prepend "0x" / "0X" for non-zero values */
            if (flag_hash && n != 0) {
                memmove(tmp + 2, tmp, (size_t)val_len + 1);
                tmp[0] = '0'; tmp[1] = (spec == 'X') ? 'X' : 'x';
                val_len += 2;
            }
            break;
        }

        case 'p': {
            unsigned long long n = (unsigned long long)(uintptr_t)va_arg(ap, void *);
            tmp[0] = '0'; tmp[1] = 'x';
            ulltoa(n, tmp + 2, 16, 0);
            val_len = (int)strlen(tmp);
            break;
        }

        default:
            /* Unknown specifier — emit literally */
            _EMIT('%'); _EMIT(spec);
            continue;
        }

        /* ── Emit numeric value with sign, zero-pad, width-pad ── */
        {
            int sign_w   = sign_ch ? 1 : 0;
            int zpad     = (prec >= 0 && prec > val_len) ? (prec - val_len) : 0;
            int content  = sign_w + zpad + val_len;
            int pad      = (width > content) ? (width - content) : 0;
            /* zero-fill only when: flag_zero set, not left-aligned, no precision */
            int use_zero = flag_zero && !flag_left && prec < 0;

            if (!flag_left && !use_zero) for (int i = 0; i < pad; i++) _EMIT(' ');
            if (sign_ch) _EMIT(sign_ch);
            if (!flag_left &&  use_zero) for (int i = 0; i < pad; i++) _EMIT('0');
            for (int i = 0; i < zpad; i++) _EMIT('0');
            _EMITS(tmp, val_len);
            if ( flag_left) for (int i = 0; i < pad; i++) _EMIT(' ');
        }
    }

#undef _EMIT
#undef _EMITS

    *out = '\0';
    va_end(ap);
    return total;
}

static inline void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    while (n--) {
        if (*p == (unsigned char)c) return (void *)p;
        p++;
    }
    return NULL;
}
/* vsnprintf — va_list variant of snprintf.
 * vprintf in libc.h calls this; add it here so string.h is self-contained.
 */
static inline int vsnprintf(char *str, size_t size, const char *fmt, va_list ap) {
    if (!str || size == 0) return 0;

    char *out = str;
    size_t rem = size - 1;
    int total  = 0;

#define _EMIT(ch) do { if (rem > 0) { *out++ = (char)(ch); rem--; } total++; } while(0)
#define _EMITS(s, n) do { for (int _i = 0; _i < (int)(n); _i++) _EMIT((s)[_i]); } while(0)

    for (const char *p = fmt; *p; ) {
        if (*p != '%') { _EMIT(*p++); continue; }
        p++; if (!*p) break;

        int flag_left=0,flag_zero=0,flag_plus=0,flag_space=0,flag_hash=0;
        for(;;){
            if(*p=='-'){flag_left=1;p++;}
            else if(*p=='0'){flag_zero=1;p++;}
            else if(*p=='+'){flag_plus=1;p++;}
            else if(*p==' '){flag_space=1;p++;}
            else if(*p=='#'){flag_hash=1;p++;}
            else break;
        }
        int width=0;
        while(*p>='0'&&*p<='9'){width=width*10+(*p++-'0');}
        int prec=-1;
        if(*p=='.'){p++;prec=0;while(*p>='0'&&*p<='9'){prec=prec*10+(*p++-'0');}}
        int lmod=0;
        if(*p=='l'){lmod=1;p++;if(*p=='l'){lmod=2;p++;}}
        else if(*p=='z'){lmod=3;p++;}

        char spec=*p++;
        char tmp[68]; int val_len=0; char sign_ch=0;

        if(spec=='s'){
            const char *s=va_arg(ap,const char*); if(!s)s="(null)";
            int slen=(int)strlen(s); if(prec>=0&&slen>prec)slen=prec;
            int pad=(width>slen)?(width-slen):0;
            if(!flag_left)for(int i=0;i<pad;i++)_EMIT(' ');
            _EMITS(s,slen);
            if(flag_left)for(int i=0;i<pad;i++)_EMIT(' ');
            continue;
        }
        if(spec=='c'){
            char c=(char)va_arg(ap,int);
            int pad=(width>1)?(width-1):0;
            if(!flag_left)for(int i=0;i<pad;i++)_EMIT(' ');
            _EMIT(c);
            if(flag_left)for(int i=0;i<pad;i++)_EMIT(' ');
            continue;
        }
        if(spec=='%'){_EMIT('%');continue;}

        switch(spec){
        case 'd':case 'i':{
            long long n;
            if(lmod==2)n=va_arg(ap,long long);
            else if(lmod==1)n=va_arg(ap,long);
            else if(lmod==3)n=(long long)va_arg(ap,size_t);
            else n=va_arg(ap,int);
            if(n<0){sign_ch='-';ulltoa((unsigned long long)-n,tmp,10,0);}
            else if(flag_plus){sign_ch='+';ulltoa((unsigned long long)n,tmp,10,0);}
            else if(flag_space){sign_ch=' ';ulltoa((unsigned long long)n,tmp,10,0);}
            else{ulltoa((unsigned long long)n,tmp,10,0);}
            val_len=(int)strlen(tmp); break;}
        case 'u':{
            unsigned long long n;
            if(lmod==2)n=va_arg(ap,unsigned long long);
            else if(lmod==1)n=va_arg(ap,unsigned long);
            else if(lmod==3)n=va_arg(ap,size_t);
            else n=(unsigned long long)va_arg(ap,unsigned int);
            ulltoa(n,tmp,10,0); val_len=(int)strlen(tmp); break;}
        case 'o':{
            unsigned long long n;
            if(lmod==2)n=va_arg(ap,unsigned long long);
            else if(lmod==1)n=va_arg(ap,unsigned long);
            else n=(unsigned long long)va_arg(ap,unsigned int);
            ulltoa(n,tmp,8,0); val_len=(int)strlen(tmp);
            if(flag_hash&&tmp[0]!='0'){memmove(tmp+1,tmp,val_len+1);tmp[0]='0';val_len++;}
            break;}
        case 'x':case 'X':{
            unsigned long long n;
            if(lmod==2)n=va_arg(ap,unsigned long long);
            else if(lmod==1)n=va_arg(ap,unsigned long);
            else if(lmod==3)n=va_arg(ap,size_t);
            else n=(unsigned long long)va_arg(ap,unsigned int);
            ulltoa(n,tmp,16,spec=='X'); val_len=(int)strlen(tmp);
            if(flag_hash&&n!=0){memmove(tmp+2,tmp,val_len+1);tmp[0]='0';tmp[1]=(spec=='X')?'X':'x';val_len+=2;}
            break;}
        case 'p':{
            unsigned long long n=(unsigned long long)(uintptr_t)va_arg(ap,void*);
            tmp[0]='0';tmp[1]='x';ulltoa(n,tmp+2,16,0);val_len=(int)strlen(tmp);break;}
        default: _EMIT('%'); _EMIT(spec); continue;
        }

        {
            int sign_w=sign_ch?1:0;
            int zpad=(prec>=0&&prec>val_len)?(prec-val_len):0;
            int content=sign_w+zpad+val_len;
            int pad=(width>content)?(width-content):0;
            int use_zero=flag_zero&&!flag_left&&prec<0;
            if(!flag_left&&!use_zero)for(int i=0;i<pad;i++)_EMIT(' ');
            if(sign_ch)_EMIT(sign_ch);
            if(!flag_left&&use_zero)for(int i=0;i<pad;i++)_EMIT('0');
            for(int i=0;i<zpad;i++)_EMIT('0');
            _EMITS(tmp,val_len);
            if(flag_left)for(int i=0;i<pad;i++)_EMIT(' ');
        }
    }

#undef _EMIT
#undef _EMITS

    *out = '\0';
    va_end(ap);
    return total;
}