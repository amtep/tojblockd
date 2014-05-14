/*
 * Copyright (C) 2013-2014 Jolla Ltd.
 * Contact: Richard Braakman <richard.braakman@jollamobile.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "helpers.h"

#include <unistd.h>

#include <sys/mman.h>

namespace QTest {
template<unsigned char> char *toString(const unsigned char &c) {
    char *str;
    if (c >= 32 && c < 127)
        asprintf(&str, "'%c' (%d)", c, (int) c);
    else
        asprintf(&str, "%d", (int) c);
    return str;
}
}

static size_t g_pagesize;

void *alloc_guarded(size_t size) {
    char *p;
    size_t adj;
    size_t length;

    if (!g_pagesize)
        g_pagesize = sysconf(_SC_PAGESIZE);

    // The goal here is to allocate the requested area in such a way
    // that there is an unreadable/unwritable page right where it ends.
    // If the size is page-aligned, then also allocate an inaccessible
    // page before its start.
    // The mapped length is stored at the start of the mapped area,
    // for use by free_guarded() later. If there's no room there, then
    // add a page to the mapping.

    adj = g_pagesize - (size % g_pagesize);
    if (adj < sizeof(size_t))
        adj += g_pagesize;

    length = adj + size + g_pagesize;
    p = (char *) mmap(NULL, length, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    * (size_t *) p = length; // record this for free_guarded()
    if (adj >= g_pagesize)
        mprotect(p, g_pagesize, PROT_NONE);
    mprotect(p + adj + size, g_pagesize, PROT_NONE);
    return p + adj;
}

void free_guarded(void *p) {
    char *base;
    if (!p)
        return;

    base = ((char *) p) - ((intptr_t) p % g_pagesize);
    if ((char *) p - base <= (int) sizeof(size_t)) {
        base -= g_pagesize;
        // make the size information readable again
        mprotect(base, g_pagesize, PROT_READ);
    }
    munmap(base, * (size_t *) base);
}
