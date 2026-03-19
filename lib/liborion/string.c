// string.c - liborion string and conversion functions

#include "orion.h"

// memory copy
void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

// memory fill
void *memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

// memory compare
int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = (const uint8_t *)a;
    const uint8_t *q = (const uint8_t *)b;
    while (n--) {
        if (*p != *q) return (int)*p - (int)*q;
        p++; q++;
    }
    return 0;
}

// string length
size_t strlen(const char *s) {
    size_t n = 0;
    while (*s++) n++;
    return n;
}

// string length until # characters
size_t  strnlen (const char *s, size_t maxlen) {
    size_t n = 0;
    while (n < maxlen && s[n]) n++;
    return n;
}

// copy string
char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

// copy # characters between strings
char *strncpy(char *dst, const char *src, size_t n) {
    char *d = dst;
    while (n && (*d++ = *src++)) n--;
    while (n--) *d++ = '\0';
    return dst;
}

// compare two strings
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

// compare # characters between strings
int strncmp(const char *a, const char *b, size_t n) {
    while (n-- && *a && *a == *b) { a++; b++; }
    if (!n) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

// find character
char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : 0;
}

// concatenate
char *strcat(char *dst, const char *src) {
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++));
    return dst;
}

// concatenate until # characters
char   *strncat (char *dst, const char *src, size_t n) {
    char *d = dst;
    while (*d) d++;
    while (n-- && *src) *d++ = *src++;
    *d = '\0';
    return dst;
}

// find first "needle" in "haystack"
char   *strstr  (const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    size_t nlen = strlen(needle);
    while (*haystack) {
        if (strncmp(haystack, needle, nlen) == 0) {
            return (char *)haystack;
        }
        haystack++;
    }
    return 0;
}

// tokenise string
char   *strtok  (char *str, const char *delim) {
    static char *saved = 0;
    if (str) saved = str;
    if (!saved || !*saved) return 0;

    while (*saved && strchr(delim, *saved)) saved++;    // skip leading delimiters
    if (!*saved) return 0;

    char *token = saved;
    while (*saved && !strchr(delim, *saved)) saved++;   // walk to end of token
    if (*saved) *saved++ = '\0';                        // null-terminate and advance

    return token;
}

// duplicate string
char   *strdup  (const char *s) {
    uint32_t len = (uint32_t)strlen(s) + 1;
    char *copy = malloc(len);
    if (copy) memcpy(copy, s, len);
    return copy;
}

// ascii -> integer
int atoi(const char *s) {
    int n    = 0;
    int sign = 1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return sign * n;
}

// integer -> ascii
char *itoa(int value, char *buf, int base) {
    static const char digits[] = "0123456789abcdef";
    char tmp[34];
    int  i = 32;
    int  neg = 0;
    tmp[33] = '\0';

    if (base < 2 || base > 16) { buf[0] = '\0'; return buf; }

    if (value < 0 && base == 10) {
        neg = 1;
        value = -value;
    }

    uint32_t uval = (uint32_t)value;
    if (uval == 0) {
        tmp[i--] = '0';
    } else {
        while (uval > 0) {
            tmp[i--] = digits[uval % (uint32_t)base];
            uval /= (uint32_t)base;
        }
    }
    if (neg) tmp[i--] = '-';

    strcpy(buf, &tmp[i + 1]);
    return buf;
}

