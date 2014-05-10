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

#include "dir.h"
#include "fat.h"
#include "vfat.h" // for filemap_fill

#include <errno.h>

#include <QtTest/QtTest>

#include "../helpers.h"

// stub for linking with fat.cpp
int filemap_fill(char *, uint32_t, int, uint32_t) {
    return EINVAL;
}

// Helper macros. These have to be macros because QCOMPARE can only be
// used from the test function itself.

// Call dir_fill with overflow protection. Declares buf on the stack.
#define call_dir_fill(buf, len, dir_index, offset) \
    char buf[len + 1]; \
    buf[len] = 'D'; \
    { int _ret = dir_fill(buf, len, dir_index, offset); \
      QCOMPARE(buf[len], 'D'); \
      QCOMPARE(_ret, 0); \
    }

class TestDir : public QObject {
    Q_OBJECT

    const static uint32_t DATA_CLUSTERS = 1000000;

private slots:
    void init() {
        fat_init(DATA_CLUSTERS);
        dir_init();
    }

    void test_empty_root() {
        call_dir_fill(buf, 4096, 0, 0);
        VERIFY_ARRAY(buf, 0, 4096, (char) 0); // root is still empty
        // any other index should fail
        QVERIFY(dir_fill(buf, 4096, 1, 0) != 0);
    }

    // A directory should not fill more than its requested length
    void test_partial_fill() {
        // The call_dir_fill macro will check that it didn't go out of bounds
        call_dir_fill(buf, 2000, 0, 1000);
        VERIFY_ARRAY(buf, 0, 2000, (char) 0); 
    }
};

QTEST_APPLESS_MAIN(TestDir)
#include "tst_dir.moc"
