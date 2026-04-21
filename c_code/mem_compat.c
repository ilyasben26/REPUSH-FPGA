/* Copyright 2026.  Minimal C runtime compatibility helpers for bare-metal build. */

#include <stddef.h>

void *memset(void *dst, int value, size_t n)
{
    unsigned char *p = (unsigned char *)dst;
    unsigned char v = (unsigned char)value;

    while (n > 0)
    {
        *p++ = v;
        n--;
    }

    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    while (n > 0)
    {
        *d++ = *s++;
        n--;
    }

    return dst;
}
