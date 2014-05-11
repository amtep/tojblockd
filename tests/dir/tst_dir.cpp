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

#include <stdlib.h>
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

static filename_t expand_name(const char *name) {
    int len = strlen(name) + 1; // include the trailing 0
    filename_t fname;

    for (int i = 0; i < len; i++) {
        fname.push_back((uint16_t) name[i]);
    }

    return fname;
}

// These values are the ones used to construct the
// expected short entry below
static const uint32_t test_clust = 0x20042448;
static const uint32_t test_file_size = 0x10031337;
static const uint32_t test_mtime = 0x536b4b33;
static const uint32_t test_atime = 0x536e589b;

// dir.cpp generates its short entries in a predictable
// pattern, so the expected checksums can be precalculated.
static const unsigned char short_1_checksum = 212;
static const unsigned char short_2_checksum = 213;

static const unsigned char short_entry_expect[32] = {
    // invalidated short name
    ' ', 0, 1, 0, 0, 0, 0, 0, '/', 0, 0,
    0x01, // read only
    0,
    100, // fine resolution of mtime (1 second)
    0xef, 0x41, // mtime: 08:15:30
    0xa8, 0x44, // mtime: May 8 2014
    0xaa, 0x44, // atime: May 10 2014
    0x04, 0x20, // two MSB of cluster number
    0xef, 0x41, // mtime: 08:15:30
    0xa8, 0x44, // mtime: May 8 2014
    0x48, 0x24, // two LSB of cluster number
    0x37, 0x13, 0x03, 0x10 // file size
};

static unsigned char dir_entry_expect[32] = {
    // this is all the same as short_entry_expect except marked fields
    ' ', 0, 1, 0, 0, 0, 0, 0, '/', 0, 0,
    0x11, // read only directory
    0,
    100,
    0xef, 0x41,
    0xa8, 0x44,
    0xaa, 0x44,
    0, 0, // two MSB of cluster number, caller must fill
    0xef, 0x41,
    0xa8, 0x44,
    0, 0, // two LSB of cluster number, caller must fill
    0, 0, 0, 0 // file size
};

static unsigned char short_entry_2_expect[32] = {
    // this is all the same as short_entry_expect except marked fields
    ' ', 0, 2, 0, 0, 0, 0, 0, '/', 0, 0,  // short name counter 2
    0x01,
    0,
    100,
    0xef, 0x41,
    0xa8, 0x44,
    0xaa, 0x44,
    0x04, 0x20,
    0xef, 0x41,
    0xa8, 0x44,
    0x48, 0x24,
    0x37, 0x13, 0x03, 0x10
};

// LFN for "testname.tst"
static unsigned char lfn_entry_1_expect[32] = {
    // long name, encoded in one entry
    0x41, // sequence number + start indicator
    't', 0, 'e', 0, 's', 0, 't', 0, 'n', 0,
    0x0f, // attributes for LFN entry
    0,
    0, // checksum of expected short entry, caller must fill
    'a', 0, 'm', 0, 'e', 0, '.', 0, 't', 0, 's', 0,
    0, 0,
    't', 0, 0, 0
};

// LFN for "subdir"
static unsigned char lfn_entry_2_expect[32] = {
    // long name, encoded in one entry
    0x41, // sequence number + start indicator
    's', 0, 'u', 0, 'b', 0, 'd', 0, 'i', 0,
    0x0f, // attributes for LFN entry
    0,
    0, // checksum of expected short entry, caller must fill
    'r', 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0, 0,
    0xff, 0xff, 0xff, 0xff
};

class TestDir : public QObject {
    Q_OBJECT

    const static uint32_t DATA_CLUSTERS = 1000000;

private slots:
    void init() {
        fat_init(DATA_CLUSTERS);
        dir_init();
        setenv("TZ", "UTC+1", true); // ensure consistent results from localtime
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

    // Try creating one file in the root directory
    void test_dir_entry() {
        QVERIFY(dir_add_entry(0, test_clust, expand_name("testname.tst"),
                test_file_size, FAT_ATTR_READ_ONLY, test_mtime, test_atime));
        call_dir_fill(buf, 4096, 0, 0);
        lfn_entry_1_expect[13] = short_1_checksum;
        COMPARE_ARRAY((unsigned char *) buf, lfn_entry_1_expect, 32);
        COMPARE_ARRAY((unsigned char *) buf + 32, short_entry_expect, 32);
        VERIFY_ARRAY(buf, 64, 4096, (char) 0);
    }

    // Try creating a subdirectory of the root,
    // and then creating a file entry in the subdirectory.
    void test_create_subdir() {
        uint32_t dir_clust = dir_alloc_new("subdir");
        QVERIFY(dir_add_entry(0, dir_clust, expand_name("subdir"),
                test_file_size, FAT_ATTR_DIRECTORY | FAT_ATTR_READ_ONLY,
                test_mtime, test_atime));
        call_dir_fill(root, 4096, 0, 0);
        dir_entry_expect[26] = dir_clust;
        dir_entry_expect[27] = dir_clust >> 8;
        dir_entry_expect[20] = dir_clust >> 16;
        dir_entry_expect[21] = dir_clust >> 24;
        lfn_entry_2_expect[13] = short_1_checksum;
        COMPARE_ARRAY((unsigned char *) root, lfn_entry_2_expect, 32);
        COMPARE_ARRAY((unsigned char *) root + 32, dir_entry_expect, 32);
        VERIFY_ARRAY(root, 64, 4096, (char) 0);

        QVERIFY(dir_add_entry(dir_clust, test_clust,
                expand_name("testname.tst"), test_file_size,
                FAT_ATTR_READ_ONLY, test_mtime, test_atime));
        call_dir_fill(subdir, 4096, 1, 0);
        lfn_entry_1_expect[13] = short_2_checksum;
        COMPARE_ARRAY((unsigned char *) subdir, lfn_entry_1_expect, 32);
        COMPARE_ARRAY((unsigned char *) subdir + 32, short_entry_2_expect, 32);
        VERIFY_ARRAY(subdir, 64, 4096, (char) 0);
    }

    // Try creating a directory entry with a name that has to be
    // split over multiple LFN entries. For good measure, test the
    // edge case where the final null character needs its own entry.
    void test_create_long_name() {
    }

    // Try filling up a directory so that it has to expand to
    // an extra cluster.
    void test_large_dir() {
    }

    void test_bad_input() {
    }
};

QTEST_APPLESS_MAIN(TestDir)
#include "tst_dir.moc"
