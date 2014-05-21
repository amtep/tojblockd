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
#include "filemap.h"  // for filemap_fill prototype
#include "dir.h" // for dir_fill prototype

#include "fat_check.h"

#include <errno.h>

#include <sys/mman.h>

#include <QtTest/QtTest>

#include "../helpers.h"

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

    // These are provided by init() for convenience of test methods
    uint32_t *fatpage;
    char *datapage;
    uint32_t filled;

private slots:
    void init() {
        fatpage = (uint32_t *) alloc_guarded(1024 * sizeof(uint32_t));
        datapage = (char *) alloc_guarded(4096);
        filled = 0;

        fat_init(DATA_CLUSTERS);
    }

    void cleanup() {
        free_guarded(fatpage);
        free_guarded(datapage);
    }

    void test_empty_fat() {
        // No dirs should be found
        QCOMPARE(fat_dir_index(0), -1);
        QCOMPARE(fat_dir_index(1000), -1);

        fat_finalize(DATA_CLUSTERS); // required by API

        fat_fill(fatpage, 0, 1024);
        // Check the special first two fat entries
        QCOMPARE(fatpage[0], (uint32_t) 0x0ffffff8); // media byte marker
        QCOMPARE(fatpage[1], (uint32_t) 0x0fffffff); // end of chain marker
        VERIFY_ARRAY(fatpage, 2, 1024, (uint32_t) 0);

        int ret = data_fill(datapage, 4096, 2, 0, &filled);
        QCOMPARE(ret, 0);
        QCOMPARE(filled, (uint32_t) 4096);
        VERIFY_ARRAY(datapage, 0, 4096, (char) 0);
    }

    // Check that the last page of the fat is correct
    void test_end_of_fat() {
        uint32_t last_page_start = FAT_ENTRIES - (FAT_ENTRIES % 1024);

        fat_finalize(DATA_CLUSTERS);

        fat_fill(fatpage, last_page_start, 1024);
        // All valid fat entries should still be 0, and the rest should
        // contain bad cluster markers.
        const int boundary = FAT_ENTRIES - last_page_start;
        VERIFY_ARRAY(fatpage, 0, boundary, (uint32_t) 0);
        VERIFY_ARRAY(fatpage, boundary, 1024, (uint32_t) 0x0ffffff7);
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
        fat_fill(fatpage, 0, 1024);
        // Check the special first two fat entries
        QCOMPARE(fatpage[0], (uint32_t) 0x0ffffff8); // media byte marker
        QCOMPARE(fatpage[1], (uint32_t) 0x0fffffff); // end of chain marker
        // Check the new directory entry
        QCOMPARE(fatpage[2], (uint32_t) 0x0fffffff); // end of chain marker
        VERIFY_ARRAY(fatpage, 3, 1024, (uint32_t) 0); // everything else 0

        // Check that data_fill accesses the directory
        int ret = data_fill(datapage, 8192, 2, 0, &filled);
        QCOMPARE(ret, 0);
        // Check that the whole directory cluster was filled
        QVERIFY2(filled >= 4096, QTest::toString(filled));
        // Check that dir_fill did the filling
        check_fill(datapage, MOCK_DIR_FILL, 4096, test_dir_index, 0);
        // and any extra fill is from an empty cluster
        // (current code does not fill more than the dir but it's allowed to)
        VERIFY_ARRAY(datapage, 4096, (int) filled, (char) 0);
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

        bool retf = fat_extend(clust_nr1, 1);
        QCOMPARE(retf, true);

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
        fat_fill(fatpage, 0, 1024);
        QCOMPARE(fatpage[0], (uint32_t) 0x0ffffff8); // media byte marker
        QCOMPARE(fatpage[1], (uint32_t) 0x0fffffff); // end of chain marker
        QCOMPARE(fatpage[2], (uint32_t) 4); // next cluster of dir 1
        QCOMPARE(fatpage[3], (uint32_t) 0x0fffffff); // end of chain marker
        QCOMPARE(fatpage[4], (uint32_t) 0x0fffffff); // end of chain marker
        VERIFY_ARRAY(fatpage, 5, 1024, (uint32_t) 0); // everything else 0

        // Check that data_fill gets it right
        int ret = data_fill(datapage, 4096, 2, 0, &filled);
        QCOMPARE(ret, 0);
        QCOMPARE(filled, (uint32_t) 4096);
        check_fill(datapage, MOCK_DIR_FILL, 4096, test_dir_index1, 0);

        ret = data_fill(datapage, 4096, 3, 0, &filled);
        QCOMPARE(ret, 0);
        QCOMPARE(filled, (uint32_t) 4096);
        check_fill(datapage, MOCK_DIR_FILL, 4096, test_dir_index2, 0);

        ret = data_fill(datapage, 4096, 4, 0, &filled);
        QCOMPARE(ret, 0);
        QCOMPARE(filled, (uint32_t) 4096);
        check_fill(datapage, MOCK_DIR_FILL, 4096, test_dir_index1, 4096);
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
        fat_fill(fatpage, 0, 1024);
        QCOMPARE(fatpage[0], (uint32_t) 0x0ffffff8); // media byte marker
        QCOMPARE(fatpage[1], (uint32_t) 0x0fffffff); // end of chain marker
        QCOMPARE(fatpage[2], (uint32_t) 4); // next cluster of dir 1
        QCOMPARE(fatpage[3], (uint32_t) 0x0fffffff); // end of chain marker
        QCOMPARE(fatpage[4], (uint32_t) 5); // next cluster of dir 1
        QCOMPARE(fatpage[5], (uint32_t) 0x0fffffff); // end of chain marker
        VERIFY_ARRAY(fatpage, 6, 1024, (uint32_t) 0); // everything else 0

        // Check that data_fill gets it right
        int ret = data_fill(datapage, 4096, 2, 0, &filled);
        QCOMPARE(ret, 0);
        QCOMPARE(filled, (uint32_t) 4096);
        check_fill(datapage, MOCK_DIR_FILL, 4096, test_dir_index1, 0);

        ret = data_fill(datapage, 4096, 3, 0, &filled);
        QCOMPARE(ret, 0);
        QCOMPARE(filled, (uint32_t) 4096);
        check_fill(datapage, MOCK_DIR_FILL, 4096, test_dir_index2, 0);

        ret = data_fill(datapage, 4096, 4, 0, &filled);
        QCOMPARE(ret, 0);
        QCOMPARE(filled, (uint32_t) 4096);
        check_fill(datapage, MOCK_DIR_FILL, 4096, test_dir_index1, 4096);

        ret = data_fill(datapage, 4096, 5, 0, &filled);
        QCOMPARE(ret, 0);
        QCOMPARE(filled, (uint32_t) 4096);
        check_fill(datapage, MOCK_DIR_FILL, 4096, test_dir_index1, 2 * 4096);
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
        uint32_t *buf = (uint32_t *) alloc_guarded(
                (test_clusters + 2) * sizeof(uint32_t));
        fat_fill(buf, expected_entry - 1, test_clusters + 2);
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
        int ret = data_fill(datapage, 4096, expected_entry + 3, 0, &filled);
        QCOMPARE(ret, 0);
        QCOMPARE(filled, (uint32_t) 4096);
        check_fill(datapage, MOCK_FILEMAP_FILL, 4096, test_filemap, 3 * 4096);
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

        int ret = data_fill(datapage, 4096, clust_nr1 - 1, 512, &filled);
        QCOMPARE(ret, 0);
        const uint32_t expected_end = 4096 - 512;
        const uint32_t expected_offset = (test_clusters - 1) * 4096 + 512;
        // data_fill is allowed to go past the end of the cluster,
        // but doesn't have to.
        QVERIFY(filled >= expected_end);
        QVERIFY(filled <= 4096);
        check_fill(datapage, MOCK_FILEMAP_FILL, expected_end, test_filemap2,
                expected_offset);
        if (filled > expected_end) {
            check_fill(datapage + expected_end, MOCK_FILEMAP_FILL,
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
        uint32_t *buf = (uint32_t *) alloc_guarded(
                FAT_ENTRIES * sizeof(uint32_t));
        fat_fill(buf, 0, FAT_ENTRIES);
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

        int ret = data_fill(datapage, 4096, FAT_ENTRIES, 0, &filled);
        QCOMPARE(ret, EINVAL);
    }

    // Try a fat_receive that extends the root dir by one cluster
    void test_write_extend_root() {
        uint32_t rootclust = fat_alloc_dir(0);
        QCOMPARE(rootclust, (uint32_t) 2);

        fat_finalize(DATA_CLUSTERS);

        // Verify initial state
        fat_fill(fatpage, 0, 1024);
        QCOMPARE(fatpage[0], (uint32_t) 0x0ffffff8); // media byte marker
        QCOMPARE(fatpage[1], (uint32_t) 0x0fffffff); // end of chain marker
        QCOMPARE(fatpage[2], (uint32_t) htole32(0x0fffffff));

        // Extend the root dir by one cluster
        fatpage[3] = fatpage[2];
        fatpage[2] = htole32(3);

        mprotect(fatpage, 4096, PROT_READ);
        int ret = fat_receive(fatpage, 0, 1024);
        mprotect(fatpage, 4096, PROT_READ | PROT_WRITE);
        QCOMPARE(ret, 0);

        QCOMPARE(fat_check_invariants(), (char *) 0);

        // Check that reading the FAT back shows the changes
        memset(fatpage, -1, 4096);
        fat_fill(fatpage, 0, 1024);
        QCOMPARE(fatpage[0], (uint32_t) 0x0ffffff8); // media byte marker
        QCOMPARE(fatpage[1], (uint32_t) 0x0fffffff); // end of chain marker
        QCOMPARE(fatpage[2], (uint32_t) 3);
        QCOMPARE(fatpage[3], (uint32_t) 0x0fffffff); // end of chain marker
        VERIFY_ARRAY(fatpage, 4, 1024, (uint32_t) 0);

        // Then check that reading the root dir now has two clusters
        ret = data_fill(datapage, 4096, 2, 0, &filled);
        QCOMPARE(ret, 0);
        QCOMPARE(filled, (uint32_t) 4096);
        check_fill(datapage, MOCK_DIR_FILL, 4096, 0, 0);

        ret = data_fill(datapage, 4096, 3, 0, &filled);
        QCOMPARE(ret, 0);
        QCOMPARE(filled, (uint32_t) 4096);
        check_fill(datapage, MOCK_DIR_FILL, 4096, 0, 4096);
    }

    // Try a fat_receive that extends the root dir by one cluster,
    // with another directory in between
    void test_write_extend_multipart() {
        uint32_t rootclust = fat_alloc_dir(0);
        QCOMPARE(rootclust, (uint32_t) 2);
        uint32_t subdirclust = fat_alloc_dir(1);
        QCOMPARE(subdirclust, (uint32_t) 3);
        fat_finalize(DATA_CLUSTERS);

        // Verify initial state
        fat_fill(fatpage, 0, 1024);
        QCOMPARE(fatpage[0], (uint32_t) 0x0ffffff8); // media byte marker
        QCOMPARE(fatpage[1], (uint32_t) 0x0fffffff); // end of chain marker
        QCOMPARE(fatpage[2], (uint32_t) htole32(0x0fffffff)); // root
        QCOMPARE(fatpage[3], (uint32_t) htole32(0x0fffffff)); // subdir
        VERIFY_ARRAY(fatpage, 4, 1024, (uint32_t) 0);

        // Extend the root dir by one cluster
        fatpage[4] = fatpage[2];
        fatpage[2] = htole32(4);

        mprotect(fatpage, 4096, PROT_READ);
        int ret = fat_receive(fatpage, 0, 1024);
        mprotect(fatpage, 4096, PROT_READ | PROT_WRITE);
        QCOMPARE(ret, 0);
        QCOMPARE(fat_check_invariants(), (char *) 0);

        // Check that reading the FAT back shows the changes
        memset(fatpage, -1, 4096);
        fat_fill(fatpage, 0, 1024);
        QCOMPARE(fatpage[0], (uint32_t) 0x0ffffff8); // media byte marker
        QCOMPARE(fatpage[1], (uint32_t) 0x0fffffff); // end of chain marker
        QCOMPARE(fatpage[2], (uint32_t) 4);
        QCOMPARE(fatpage[3], (uint32_t) 0x0fffffff); // end of chain marker
        QCOMPARE(fatpage[4], (uint32_t) 0x0fffffff); // end of chain marker
        VERIFY_ARRAY(fatpage, 5, 1024, (uint32_t) 0);

        // Then check that reading the root dir now has two clusters
        ret = data_fill(datapage, 4096, 2, 0, &filled);
        QCOMPARE(ret, 0);
        QCOMPARE(filled, (uint32_t) 4096);
        check_fill(datapage, MOCK_DIR_FILL, 4096, 0, 0);

        ret = data_fill(datapage, 4096, 4, 0, &filled);
        QCOMPARE(ret, 0);
        QCOMPARE(filled, (uint32_t) 4096);
        check_fill(datapage, MOCK_DIR_FILL, 4096, 0, 4096);

        // And check that the subdir was left alone
        ret = data_fill(datapage, 4096, 3, 0, &filled);
        QCOMPARE(ret, 0);
        QCOMPARE(filled, (uint32_t) 4096);
        check_fill(datapage, MOCK_DIR_FILL, 4096, 1, 0);
    }
};

QTEST_APPLESS_MAIN(TestFat)
#include "tst_fat.moc"
