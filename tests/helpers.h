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
