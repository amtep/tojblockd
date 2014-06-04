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

#include <stdlib.h>
#include <errno.h>

#include <QtTest/QtTest>

#include "fat.h"
#include "image.h"

#include "../helpers.h"

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

// LFN for "abcdefghijklmnopqrstuvwxyz"
static unsigned char lfn_entry_3_expect[32 * 3] = {
    // long name, encoded in three entries, last part first
    0x43, // sequence number + start indicator
    // the name must have a terminating nul even if it means
    // allocating another entry for it
    0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x0f,
    0,
    0, // checksum of expected short entry, caller must fill
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0, 0,
    0xff, 0xff, 0xff, 0xff,

    0x02,
    'n', 0, 'o', 0, 'p', 0, 'q', 0, 'r', 0,
    0x0f,
    0,
    0, // checksum of expected short entry, caller must fill
    's', 0, 't', 0, 'u', 0, 'v', 0, 'w', 0, 'x', 0,
    0, 0,
    'y', 0, 'z', 0,

    0x01,
    'a', 0, 'b', 0, 'c', 0, 'd', 0, 'e', 0,
    0x0f, // attributes for LFN entry
    0,
    0, // checksum of expected short entry, caller must fill
    'f', 0, 'g', 0, 'h', 0, 'i', 0, 'j', 0, 'k', 0,
    0, 0,
    'l', 0, 'm', 0
};

class TestDir : public QObject {
    Q_OBJECT

    const static uint32_t DATA_CLUSTERS = 1000000;

    char *page;

private slots:
    void init() {
        page = (char *) alloc_guarded(4096);
        setenv("TZ", "UTC+1", true); // ensure consistent results from localtime

        image_init();
        fat_init(DATA_CLUSTERS);
        dir_init();
    }

    void cleanup() {
        free_guarded(page);
    }

    void test_empty_root() {
        int ret = image_fill(page, fat_cluster_pos(2), 4096);
        QCOMPARE(ret, 0);
        VERIFY_ARRAY(page, 0, 4096, (char) 0); // root is still empty
    }

    // A directory should not fill more than its requested length
    void test_partial_fill() {
        char *buf = (char *) alloc_guarded(2000);
        int ret = image_fill(buf, fat_cluster_pos(2) + 1000, 2000);
        QCOMPARE(ret, 0);
        VERIFY_ARRAY(buf, 0, 2000, (char) 0);
        free_guarded(buf);
    }

    // Try creating one file in the root directory
    void test_dir_entry() {
        QVERIFY(dir_add_entry(0, test_clust, expand_name("testname.tst"),
                test_file_size, FAT_ATTR_READ_ONLY, test_mtime, test_atime));
        int ret = image_fill(page, fat_cluster_pos(2), 4096);
        QCOMPARE(ret, 0);
        lfn_entry_1_expect[13] = short_1_checksum;
        COMPARE_ARRAY((unsigned char *) page, lfn_entry_1_expect, 32);
        COMPARE_ARRAY((unsigned char *) page + 32, short_entry_expect, 32);
        VERIFY_ARRAY(page, 64, 4096, (char) 0);
    }

    // Try creating a subdirectory of the root,
    // and then creating a file entry in the subdirectory.
    void test_create_subdir() {
        uint32_t dir_clust = dir_alloc_new("subdir");
        QVERIFY(dir_add_entry(0, dir_clust, expand_name("subdir"),
                test_file_size, FAT_ATTR_DIRECTORY | FAT_ATTR_READ_ONLY,
                test_mtime, test_atime));
        int ret = image_fill(page, fat_cluster_pos(2), 4096);
        QCOMPARE(ret, 0);
        dir_entry_expect[26] = dir_clust;
        dir_entry_expect[27] = dir_clust >> 8;
        dir_entry_expect[20] = dir_clust >> 16;
        dir_entry_expect[21] = dir_clust >> 24;
        lfn_entry_2_expect[13] = short_1_checksum;
        COMPARE_ARRAY((unsigned char *) page, lfn_entry_2_expect, 32);
        COMPARE_ARRAY((unsigned char *) page + 32, dir_entry_expect, 32);
        VERIFY_ARRAY(page, 64, 4096, (char) 0);

        QVERIFY(dir_add_entry(dir_clust, test_clust,
                expand_name("testname.tst"), test_file_size,
                FAT_ATTR_READ_ONLY, test_mtime, test_atime));
        ret = image_fill(page, fat_cluster_pos(dir_clust), 4096);
        QCOMPARE(ret, 0);
        lfn_entry_1_expect[13] = short_2_checksum;
        COMPARE_ARRAY((unsigned char *) page, lfn_entry_1_expect, 32);
        COMPARE_ARRAY((unsigned char *) page + 32, short_entry_2_expect, 32);
        VERIFY_ARRAY(page, 2 * 32, 4096, (char) 0);
    }

    // Try creating a directory entry with a name that has to be
    // split over multiple LFN entries. For good measure, test the
    // edge case where the final null character needs its own entry.
    void test_create_long_name() {
        const char *name = "abcdefghijklmnopqrstuvwxyz";
        QVERIFY(dir_add_entry(0, test_clust, expand_name(name),
            test_file_size, FAT_ATTR_READ_ONLY, test_mtime, test_atime));
        int ret = image_fill(page, fat_cluster_pos(2), 4096);
        QCOMPARE(ret, 0);
        lfn_entry_3_expect[13] = short_1_checksum;
        lfn_entry_3_expect[13 + 32] = short_1_checksum;
        lfn_entry_3_expect[13 + 2 * 32] = short_1_checksum;
        COMPARE_ARRAY((unsigned char *) page, lfn_entry_3_expect, 32 * 3);
        COMPARE_ARRAY((unsigned char *) page + 3 * 32,
                short_entry_expect, 32);
        VERIFY_ARRAY(page, 4 * 32, 4096, (char) 0);
    }

    // Try filling up a directory so that it has to expand to
    // an extra cluster.
    void test_large_dir() {
        char name[20];
        int i;

        // fill up the first cluster (but don't go over yet)
        // each file takes up two 32-byte entries
        for (i = 0; i < 4096 / (2 * 32); i++) {
            sprintf(name, "testname%d", i);
            QVERIFY(dir_add_entry(0, test_clust + i, expand_name(name),
                    test_file_size, FAT_ATTR_READ_ONLY, test_mtime, test_atime));
        }
        QCOMPARE(fat_alloc_beginning(1), (uint32_t) 3); // cluster 3 was free

        // this call should expand the directory in the FAT
        QVERIFY(dir_add_entry(0, test_clust + i++, expand_name("testname.tst"),
                test_file_size, FAT_ATTR_READ_ONLY, test_mtime, test_atime));
        QCOMPARE(fat_alloc_beginning(1), (uint32_t) 5); // cluster 4 was taken

        // now try it again with the second cluster
        // (regression test for a bug where it started allocating
        // a new cluster for every entry)
        for ( ; i < 2 * 4096 / (2 * 32); i++) {
            sprintf(name, "testname%d", i);
            QVERIFY(dir_add_entry(0, test_clust + i, expand_name(name),
                    test_file_size, FAT_ATTR_READ_ONLY, test_mtime, test_atime));
        }
        QCOMPARE(fat_alloc_beginning(1), (uint32_t) 6); // cluster 6 was free

        // this call should expand the directory in the FAT again
        QVERIFY(dir_add_entry(0, test_clust + i++, expand_name("test2.tst"),
                test_file_size, FAT_ATTR_READ_ONLY, test_mtime, test_atime));
        QCOMPARE(fat_alloc_beginning(1), (uint32_t) 8); // cluster 7 was taken

        // Check if the tail of the dir is indeed in cluster 7
        int ret = image_fill(page, fat_cluster_pos(7), 4096);
        QCOMPARE(ret, 0);
        // quick check for start of LFN
	QCOMPARE((unsigned char) (page[0]), (unsigned char) 0x41);
        // Quick check if shortname counter has seen the right nr of names
	QCOMPARE((unsigned char) (page[32 + 2]), (unsigned char) (i & 0x1f));
    }

    void test_bad_input() {
        // Add entry to nonexistent dir
        QCOMPARE(dir_add_entry(1, test_clust, expand_name("testname.tst"),
                test_file_size, FAT_ATTR_READ_ONLY, test_mtime, test_atime),
                false);
    }

    void test_overlong_name() {
        // FAT filesystem spec allows a maximum of 255-character names
        char name[256];
        memset(name, 'a', 256);
        QCOMPARE(dir_add_entry(0, test_clust, expand_name(name),
                test_file_size, FAT_ATTR_READ_ONLY, test_mtime, test_atime),
                false);
        name[255] = 0; // shorten it to allowed length, should work now
        QVERIFY(dir_add_entry(0, test_clust, expand_name(name),
                test_file_size, FAT_ATTR_READ_ONLY, test_mtime, test_atime));
    }
};

QTEST_APPLESS_MAIN(TestDir)
#include "tst_dir.moc"
