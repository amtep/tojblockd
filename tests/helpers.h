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

namespace QTest {
template<> inline char *toString(const unsigned char &c) {
    char *str;
    if (c >= 32 && c < 127)
        asprintf(&str, "'%c' (%d)", c, (int) c);
    else
        asprintf(&str, "%d", (int) c);
    return str;
}
}
