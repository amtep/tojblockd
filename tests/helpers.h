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

#include <stdio.h>

#include <QtTest/QtTest>

// Helper macros for common operations in test methods.

#define VERIFY_ARRAY(array, start, limit, expected) \
    do { \
        for (int _i = (start); _i < (limit); ++_i) { \
            if (!((array)[_i] == (expected))) { \
                char *_actualstr = 0; \
                asprintf(&_actualstr, "%s[%d]", #array, _i); \
                if (!QTest::qCompare((array)[_i], (expected), \
                        _actualstr, #expected, __FILE__, __LINE__)) { \
                    free(_actualstr); \
                    return; \
                } \
                free(_actualstr); \
            } \
        } \
    } while (0);

#define COMPARE_ARRAY(actual, expected, limit) \
    do { \
        for (int _i = 0; _i < (limit); ++_i) { \
            if (!((actual)[_i] == (expected)[_i])) { \
                char *_actualstr = 0; \
                char *_expectedstr = 0; \
                asprintf(&_actualstr, "%s[%d]", #actual, _i); \
                asprintf(&_expectedstr, "%s[%d]", #expected, _i); \
                if (!QTest::qCompare((actual)[_i], (expected)[_i], \
                        _actualstr, _expectedstr, __FILE__, __LINE__)) { \
                    free(_actualstr); \
                    free(_expectedstr); \
                    return; \
                } \
                free(_actualstr); \
                free(_expectedstr); \
            } \
        } \
    } while (0);

// Make sure QCOMPARE compares the pointers instead of what they point to.
// (which tends to be a problem with char * buffers).
#define COMPARE_POINTERS(actual, expected) \
    QCOMPARE((void *)(actual), (void *)(expected))

namespace QTest {
template<> char *toString<unsigned char>(const unsigned char &);
}

/*
 * Allocate a block in such a way that reads or writes that go past
 * the end of it will result in a segmentation fault.
 * If the size is a multiple of the page size, then reads or writes
 * before the start of it will be guarded too.
 * Since this function has to allocate its block at the end of
 * a page, it doesn't give the same alignment guarantees as malloc
 * does. The alignment of the returned pointer will depend on
 * the alignment of the size.
 */
void *alloc_guarded(size_t size);

/* Free a block that was allocated with alloc_guarded */
void free_guarded(void *p);
