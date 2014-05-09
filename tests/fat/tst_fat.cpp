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

#include "fat.h"
#include "vfat.h"  // for filemap_fill
#include "dir.h" // for dir_fill

#include <errno.h>

#include <QtTest/QtTest>

// Mock helper
struct fill_struct {
    int type;
    uint32_t len;
    int index;
    uint32_t offset;
};

enum {
    MOCK_FILEMAP_FILL,
    MOCK_DIR_FILL,
};

// Mock function
int filemap_fill(char *buf, uint32_t len, int fmap_index, uint32_t offset)
{
    memset(buf, (char) fmap_index, len);

    struct fill_struct *f = (struct fill_struct *) buf;
    if (len >= sizeof(fill_struct)) {
        f->type = MOCK_FILEMAP_FILL;
        f->len = len;
        f->index = fmap_index;
        f->offset = offset;
    }

    return 0;
}

// Mock function
int dir_fill(char *buf, uint32_t len, int dir_index, uint32_t offset)
{
    memset(buf, (char) dir_index, len);

    struct fill_struct *f = (struct fill_struct *) buf;
    if (len >= sizeof(fill_struct)) {
        f->type = MOCK_DIR_FILL;
        f->len = len;
        f->index = dir_index;
        f->offset = offset;
    }

    return 0;
}

// Helper macros. These have to be macros because QCOMPARE can only be
// used from the test function itself.

// Call fat_fill with overflow protection. Declares buf on the stack.
#define call_fat_fill(buf, entry_nr, entries) \
    uint32_t buf[(entries) + 1]; \
    buf[entries] = 0x31337; \
    fat_fill(buf, entry_nr, entries); \
    QCOMPARE(buf[entries], (uint32_t) 0x31337);

// Call data_fill with overflow protection and checks the return code.
// Declares buf on the stack.
// Caller is responsible for declaring and checking the 'filled' parameter.
#define call_data_fill(buf, len, start_clust, offset, filled) \
    char buf[(len) + 1]; \
    buf[len] = 'X'; \
    { int _ret = data_fill(buf, len, start_clust, offset, filled); \
      QCOMPARE(buf[len], 'X'); \
      QCOMPARE(_ret, 0); \
    }

#define check_fill(buf, _type, _len, _index, _offset) \
    do { \
        struct fill_struct *_f = (struct fill_struct *) (buf); \
        QCOMPARE(_f->type, (int) (_type)); \
        QCOMPARE(_f->len, (uint32_t) (_len)); \
        QCOMPARE(_f->index, (int) (_index)); \
        QCOMPARE(_f->offset, (uint32_t) (_offset)); \
        QCOMPARE((buf)[(_len) - 1], (char) (_index)); \
    } while (0)

class TestFat : public QObject {
    Q_OBJECT

    static const uint32_t DATA_CLUSTERS = 1000000;
    static const uint32_t FAT_ENTRIES = DATA_CLUSTERS + 2;

private slots:
    void init() {
        fat_init(DATA_CLUSTERS);
    }

    void test_empty_fat() {
        // No dirs should be found
        QCOMPARE(fat_dir_index(0), -1);
        QCOMPARE(fat_dir_index(1000), -1);

        fat_finalize(DATA_CLUSTERS); // required by API

        call_fat_fill(buf, 0, 1024);
        // Check the special first two fat entries
        QCOMPARE(buf[0], (uint32_t) 0x0ffffff8); // media byte marker
        QCOMPARE(buf[1], (uint32_t) 0x0fffffff); // end of chain marker
        // everything else should be 0
        for (uint32_t i = 2; i < 1024; i++)
            QCOMPARE(buf[i], (uint32_t) 0);

        uint32_t filled;
        call_data_fill(data, 4096, 2, 0, &filled);
        QCOMPARE(filled, (uint32_t) 4096);
        // everything should be 0
        for (uint32_t i = 0; i < 4096; i++)
            QCOMPARE(data[i], (char) 0);
    }

    // Check that the last page of the fat is correct
    void test_end_of_fat() {
        uint32_t last_page_start = FAT_ENTRIES - (FAT_ENTRIES % 1024);

        fat_finalize(DATA_CLUSTERS);

        call_fat_fill(buf, last_page_start, 1024);
        // All valid fat entries should still be 0, and the rest should
        // contain bad cluster markers.
        for (uint32_t i = last_page_start; i < FAT_ENTRIES; i++) {
            QCOMPARE(buf[i - last_page_start], (uint32_t) 0);
        }
        for (uint32_t i = FAT_ENTRIES; i < last_page_start + 1024; i++) {
            // Bad cluster marker
            QCOMPARE(buf[i - last_page_start], (uint32_t) 0x0ffffff7);
        }
    }

    // Try allocating one directory and check the result
    void test_one_dir() {
        const int test_dir_index = 5;
        uint32_t clust_nr = fat_alloc_dir(test_dir_index);
        QCOMPARE(clust_nr, (uint32_t) 2);
        QCOMPARE(fat_dir_index(0), -1);
        QCOMPARE(fat_dir_index(1), -1);
        QCOMPARE(fat_dir_index(2), test_dir_index);
        QCOMPARE(fat_dir_index(3), -1);
        QCOMPARE(fat_dir_index(4), -1);

        fat_finalize(DATA_CLUSTERS);

        // Check that fat_finalize didn't mess up the entries
        QCOMPARE(fat_dir_index(0), -1);
        QCOMPARE(fat_dir_index(1), -1);
        QCOMPARE(fat_dir_index(2), test_dir_index);
        QCOMPARE(fat_dir_index(3), -1);
        QCOMPARE(fat_dir_index(4), -1);

        // Check that the first fat page shows the directory
        call_fat_fill(buf, 0, 1024);
        // Check the special first two fat entries
        QCOMPARE(buf[0], (uint32_t) 0x0ffffff8); // media byte marker
        QCOMPARE(buf[1], (uint32_t) 0x0fffffff); // end of chain marker
        // Check the new directory entry
        QCOMPARE(buf[2], (uint32_t) 0x0fffffff); // end of chain marker
        // Everything else should be 0
        for (uint32_t i = 3; i < 1024; i++)
            QCOMPARE(buf[i], (uint32_t) 0);

        // Check that data_fill accesses the directory
        uint32_t filled;
        call_data_fill(data, 8192, 2, 0, &filled);
        // Check that the whole directory cluster was filled
        QVERIFY2(filled >= 4096, QTest::toString(filled));
        // Check that dir_fill did the filling
        check_fill(data, MOCK_DIR_FILL, 4096, test_dir_index, 0);
        // and any extra fill is from an empty cluster
        // (current code does not fill more than the dir but it's allowed to)
        for (uint32_t i = 4096; i < filled; i++)
            QCOMPARE((int) data[i], 0);
    }

    // Try allocating two directories and then extending the first
    void test_extend_dir() {
        const int test_dir_index1 = 4;
        const int test_dir_index2 = 9;

        uint32_t clust_nr1 = fat_alloc_dir(test_dir_index1);
        uint32_t clust_nr2 = fat_alloc_dir(test_dir_index2);

        QCOMPARE(clust_nr1, (uint32_t) 2);
        QCOMPARE(clust_nr2, (uint32_t) 3);

        QCOMPARE(fat_dir_index(0), -1);
        QCOMPARE(fat_dir_index(1), -1);
        QCOMPARE(fat_dir_index(2), test_dir_index1);
        QCOMPARE(fat_dir_index(3), test_dir_index2);
        QCOMPARE(fat_dir_index(4), -1);
        QCOMPARE(fat_dir_index(5), -1);

        bool ret = fat_extend(clust_nr1, 1);
        QCOMPARE(ret, true);

        QCOMPARE(fat_dir_index(0), -1);
        QCOMPARE(fat_dir_index(1), -1);
        QCOMPARE(fat_dir_index(2), test_dir_index1);
        QCOMPARE(fat_dir_index(3), test_dir_index2);
        QCOMPARE(fat_dir_index(4), test_dir_index1);
        QCOMPARE(fat_dir_index(5), -1);

        fat_finalize(DATA_CLUSTERS);

        QCOMPARE(fat_dir_index(0), -1);
        QCOMPARE(fat_dir_index(1), -1);
        QCOMPARE(fat_dir_index(2), test_dir_index1);
        QCOMPARE(fat_dir_index(3), test_dir_index2);
        QCOMPARE(fat_dir_index(4), test_dir_index1);
        QCOMPARE(fat_dir_index(5), -1);

        // Check the first fat page
        call_fat_fill(buf, 0, 1024);
        QCOMPARE(buf[0], (uint32_t) 0x0ffffff8); // media byte marker
        QCOMPARE(buf[1], (uint32_t) 0x0fffffff); // end of chain marker
        QCOMPARE(buf[2], (uint32_t) 4); // next cluster of dir 1
        QCOMPARE(buf[3], (uint32_t) 0x0fffffff); // end of chain marker
        QCOMPARE(buf[4], (uint32_t) 0x0fffffff); // end of chain marker
        // Everything else should be 0
        for (uint32_t i = 5; i < 1024; i++)
            QCOMPARE(buf[i], (uint32_t) 0);

        // Check that data_fill gets it right
        uint32_t filled;
        call_data_fill(data1, 4096, 2, 0, &filled);
        QCOMPARE(filled, (uint32_t) 4096);
        check_fill(data1, MOCK_DIR_FILL, 4096, test_dir_index1, 0);

        call_data_fill(data2, 4096, 3, 0, &filled);
        QCOMPARE(filled, (uint32_t) 4096);
        check_fill(data2, MOCK_DIR_FILL, 4096, test_dir_index2, 0);

        call_data_fill(data3, 4096, 4, 0, &filled);
        QCOMPARE(filled, (uint32_t) 4096);
        check_fill(data3, MOCK_DIR_FILL, 4096, test_dir_index1, 4096);
    }

    // Try allocating two directories and then extending the first twice
    void test_extend_dir_twice() {
        const int test_dir_index1 = 7;
        const int test_dir_index2 = 11;

        uint32_t clust_nr1 = fat_alloc_dir(test_dir_index1);
        uint32_t clust_nr2 = fat_alloc_dir(test_dir_index2);

        QCOMPARE(clust_nr1, (uint32_t) 2);
        QCOMPARE(clust_nr2, (uint32_t) 3);

        bool ret1 = fat_extend(clust_nr1, 1);
        QCOMPARE(ret1, true);
        bool ret2 = fat_extend(clust_nr1, 1);
        QCOMPARE(ret2, true);

        QCOMPARE(fat_dir_index(0), -1);
        QCOMPARE(fat_dir_index(1), -1);
        QCOMPARE(fat_dir_index(2), test_dir_index1);
        QCOMPARE(fat_dir_index(3), test_dir_index2);
        QCOMPARE(fat_dir_index(4), test_dir_index1);
        QCOMPARE(fat_dir_index(5), test_dir_index1);
        QCOMPARE(fat_dir_index(6), -1);

        fat_finalize(DATA_CLUSTERS);

        QCOMPARE(fat_dir_index(0), -1);
        QCOMPARE(fat_dir_index(1), -1);
        QCOMPARE(fat_dir_index(2), test_dir_index1);
        QCOMPARE(fat_dir_index(3), test_dir_index2);
        QCOMPARE(fat_dir_index(4), test_dir_index1);
        QCOMPARE(fat_dir_index(5), test_dir_index1);
        QCOMPARE(fat_dir_index(6), -1);

        // Check the first fat page
        call_fat_fill(buf, 0, 1024);
        QCOMPARE(buf[0], (uint32_t) 0x0ffffff8); // media byte marker
        QCOMPARE(buf[1], (uint32_t) 0x0fffffff); // end of chain marker
        QCOMPARE(buf[2], (uint32_t) 4); // next cluster of dir 1
        QCOMPARE(buf[3], (uint32_t) 0x0fffffff); // end of chain marker
        QCOMPARE(buf[4], (uint32_t) 5); // next cluster of dir 1
        QCOMPARE(buf[5], (uint32_t) 0x0fffffff); // end of chain marker
        // Everything else should be 0
        for (uint32_t i = 6; i < 1024; i++)
            QCOMPARE(buf[i], (uint32_t) 0);

        // Check that data_fill gets it right
        uint32_t filled;
        call_data_fill(data1, 4096, 2, 0, &filled);
        QCOMPARE(filled, (uint32_t) 4096);
        check_fill(data1, MOCK_DIR_FILL, 4096, test_dir_index1, 0);

        call_data_fill(data2, 4096, 3, 0, &filled);
        QCOMPARE(filled, (uint32_t) 4096);
        check_fill(data2, MOCK_DIR_FILL, 4096, test_dir_index2, 0);

        call_data_fill(data3, 4096, 4, 0, &filled);
        QCOMPARE(filled, (uint32_t) 4096);
        check_fill(data3, MOCK_DIR_FILL, 4096, test_dir_index1, 4096);

        call_data_fill(data4, 4096, 5, 0, &filled);
        QCOMPARE(filled, (uint32_t) 4096);
        check_fill(data4, MOCK_DIR_FILL, 4096, test_dir_index1, 2 * 4096);
    }

    // Try allocating one filemap and check the result
    void test_one_filemap() {
        const int test_filemap = 8;
        const int test_clusters = 17;
        const uint32_t expected_entry = FAT_ENTRIES - test_clusters;

        uint32_t clust_nr = fat_alloc_filemap(test_filemap, test_clusters);
        // Check that filemap was allocated at the end of the image
        QCOMPARE(clust_nr, expected_entry);

        fat_finalize(DATA_CLUSTERS);

        // Check that the last fat page shows the file
        call_fat_fill(buf, expected_entry - 1, test_clusters + 2);
        QCOMPARE(buf[0], (uint32_t) 0); // empty before file
        // ascending chain except last entry
        for (uint32_t i = 0; i < test_clusters - 1; i++) {
            QCOMPARE(buf[i + 1], expected_entry + i + 1);
        }
        // end of chain marker
        QCOMPARE(buf[test_clusters], (uint32_t) 0x0fffffff);
        // bad cluster marker after file (because it's at the end of
        // allocatable space)
        QCOMPARE(buf[test_clusters + 1], (uint32_t) 0x0ffffff7);

        // Check that an arbitrary cluster can be loaded from the file
        uint32_t filled;
        call_data_fill(data, 4096, expected_entry + 3, 0, &filled);
        QCOMPARE(filled, (uint32_t) 4096);
        check_fill(data, MOCK_FILEMAP_FILL, 4096, test_filemap, 3 * 4096);
    }

    // Try allocating two filemaps and try a data_fill that
    // crosses the boundary between them
    void test_file_boundary() {
        const int test_filemap1 = 1;
        const int test_filemap2 = 2;
        const int test_clusters = 3;
        uint32_t clust_nr1 = fat_alloc_filemap(test_filemap1, test_clusters);
        uint32_t clust_nr2 = fat_alloc_filemap(test_filemap2, test_clusters);

        // Check that they were allocated next to each other
        QCOMPARE(clust_nr2, clust_nr1 - test_clusters);

        fat_finalize(DATA_CLUSTERS);

        uint32_t filled;
        call_data_fill(data, 4096, clust_nr1 - 1, 512, &filled);
        const uint32_t expected_end = 4096 - 512;
        const uint32_t expected_offset = (test_clusters - 1) * 4096 + 512;
        // data_fill is allowed to go past the end of the cluster,
        // but doesn't have to.
        QVERIFY(filled >= expected_end);
        QVERIFY(filled <= 4096);
        check_fill(data, MOCK_FILEMAP_FILL, expected_end, test_filemap2,
                expected_offset);
        if (filled > expected_end) {
            check_fill(&data[expected_end], MOCK_FILEMAP_FILL,
                    filled - expected_end, test_filemap1, 0);
        }
    }

    // Create an image with restricted free space
    void test_unusable_clusters() {
        fat_alloc_dir(1);
        fat_alloc_dir(2);
        fat_alloc_filemap(1, 10);
        fat_alloc_filemap(2, 10);
        const uint32_t allocated = 22;

        fat_finalize(DATA_CLUSTERS / 2);

        const uint32_t expect_free = DATA_CLUSTERS / 2;
        const uint32_t expect_bad = DATA_CLUSTERS - allocated - expect_free;

        // Load the whole FAT for analysis
        call_fat_fill(buf, 0, FAT_ENTRIES);
        uint32_t free_count = 0;
        uint32_t bad_count = 0;
        for (uint32_t i = 0; i < FAT_ENTRIES; i++) {
            if (buf[i] == 0)
                free_count++;
            else if (buf[i] == 0x0ffffff7)
                bad_count++;
        }
        QCOMPARE(free_count, expect_free);
        QCOMPARE(bad_count, expect_bad);
    }

    void test_bad_args() {
        QCOMPARE(fat_extend(0, 1), false);
        QCOMPARE(fat_extend(FAT_ENTRIES, 1), false);

        fat_finalize(DATA_CLUSTERS);

        char data[4096];
        uint32_t filled;
        int ret = data_fill(data, 4096, FAT_ENTRIES, 0, &filled);
        QCOMPARE(ret, EINVAL);
    }
};

QTEST_APPLESS_MAIN(TestFat)
#include "tst_fat.moc"
