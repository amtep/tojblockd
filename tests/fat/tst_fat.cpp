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

#include "fat_check.h"

#include <errno.h>

#include <sys/mman.h>

#include <QtTest/QtTest>

#include "vfat.h" // for RESERVED_SECTORS
#include "image.h"

#include "../helpers.h"

class TestFat : public QObject {
    Q_OBJECT

    static const uint32_t DATA_CLUSTERS = 1000000;
    static const uint32_t FAT_ENTRIES = DATA_CLUSTERS + 2;
    static const uint64_t FAT_START = RESERVED_SECTORS * SECTOR_SIZE;
    static const int FAT_CLUSTER = CLUSTER_SIZE / sizeof(uint32_t);

    // These are provided by init() for convenience of test methods
    uint32_t *fatbuf;

private slots:
    void init() {
        fatbuf = (uint32_t *) alloc_guarded(CLUSTER_SIZE);
        image_init();
        fat_init(DATA_CLUSTERS);
    }

    void cleanup() {
        free_guarded(fatbuf);
    }

    // The tests rely on some definitions from the code under test,
    // so check that those definitions are usable.
    void test_sane_sizes() {
        // Must have room for boot sector and fsinfo sector
        QVERIFY(RESERVED_SECTORS >= 2);
        QVERIFY(SECTOR_SIZE >= 512); // FAT32 spec
        QVERIFY(SECTOR_SIZE <= 4096); // tests break down if sector > page
        // Sector size must be a power of two
        QVERIFY((SECTOR_SIZE & (SECTOR_SIZE - 1)) == 0);
        // CLUSTER_SIZE must be a multiple of the sector size
        QCOMPARE(CLUSTER_SIZE % SECTOR_SIZE, 0);
        QVERIFY(CLUSTER_SIZE >= SECTOR_SIZE);
        // Cluster size must be a power of two
        QVERIFY((CLUSTER_SIZE & (CLUSTER_SIZE - 1)) == 0);
    }

    void test_empty_fat() {
        fat_finalize(DATA_CLUSTERS); // required by API

        image_fill((char *) fatbuf, FAT_START, CLUSTER_SIZE);
        // Check the special first two fat entries
        QCOMPARE(fatbuf[0], (uint32_t) 0x0ffffff8); // media byte marker
        QCOMPARE(fatbuf[1], (uint32_t) 0x0fffffff); // end of chain marker
        VERIFY_ARRAY(fatbuf, 2, FAT_CLUSTER, (uint32_t) 0);
    }

    // Check that the last page of the fat is correct
    void test_end_of_fat() {
        uint32_t last_sector_start = FAT_ENTRIES - (FAT_ENTRIES % 128);

        fat_finalize(DATA_CLUSTERS);

        image_fill((char *) fatbuf,
                FAT_START + last_sector_start * sizeof(uint32_t), SECTOR_SIZE);
        // All valid fat entries should still be 0, and the rest should
        // contain bad cluster markers.
        const int boundary = FAT_ENTRIES - last_sector_start;
        VERIFY_ARRAY(fatbuf, 0, boundary, (uint32_t) 0);
        VERIFY_ARRAY(fatbuf, boundary, SECTOR_SIZE / 4, (uint32_t) 0x0ffffff7);
    }

    // Try allocating one directory and check the result
    void test_one_dir() {
        uint32_t clust_nr = fat_alloc_beginning(1);
        QCOMPARE(clust_nr, (uint32_t) 2);

        fat_finalize(DATA_CLUSTERS);

        // Check that the first fat page shows the directory
        image_fill((char *) fatbuf, FAT_START, CLUSTER_SIZE);
        // Check the special first two fat entries
        QCOMPARE(fatbuf[0], (uint32_t) 0x0ffffff8); // media byte marker
        QCOMPARE(fatbuf[1], (uint32_t) 0x0fffffff); // end of chain marker
        // Check the new directory entry
        QCOMPARE(fatbuf[2], (uint32_t) 0x0fffffff); // end of chain marker
        VERIFY_ARRAY(fatbuf, 3, FAT_CLUSTER, (uint32_t) 0); // everything else 0
    }

    // Try allocating two directories and then extending the first
    void test_extend_dir() {
        uint32_t clust_nr1 = fat_alloc_beginning(1);
        uint32_t clust_nr2 = fat_alloc_beginning(1);

        QCOMPARE(clust_nr1, (uint32_t) 2);
        QCOMPARE(clust_nr2, (uint32_t) 3);

        uint32_t ret = fat_extend_chain(clust_nr1);
        QCOMPARE(ret, clust_nr2 + 1);

        fat_finalize(DATA_CLUSTERS);

        // Check the first fat page
        image_fill((char *) fatbuf, FAT_START, CLUSTER_SIZE);
        QCOMPARE(fatbuf[0], (uint32_t) 0x0ffffff8); // media byte marker
        QCOMPARE(fatbuf[1], (uint32_t) 0x0fffffff); // end of chain marker
        QCOMPARE(fatbuf[2], (uint32_t) 4); // next cluster of dir 1
        QCOMPARE(fatbuf[3], (uint32_t) 0x0fffffff); // end of chain marker
        QCOMPARE(fatbuf[4], (uint32_t) 0x0fffffff); // end of chain marker
        VERIFY_ARRAY(fatbuf, 5, FAT_CLUSTER, (uint32_t) 0); // everything else 0
    }

    // Try allocating two directories and then extending the first twice
    void test_extend_dir_twice() {
        uint32_t clust_nr1 = fat_alloc_beginning(1);
        uint32_t clust_nr2 = fat_alloc_beginning(1);

        QCOMPARE(clust_nr1, (uint32_t) 2);
        QCOMPARE(clust_nr2, (uint32_t) 3);

        uint32_t ret1 = fat_extend_chain(clust_nr1);
        QCOMPARE(ret1, clust_nr2 + 1);
        uint32_t ret2 = fat_extend_chain(clust_nr1);
        QCOMPARE(ret2, ret1 + 1);

        fat_finalize(DATA_CLUSTERS);

        // Check the first fat page
        image_fill((char *) fatbuf, FAT_START, CLUSTER_SIZE);
        QCOMPARE(fatbuf[0], (uint32_t) 0x0ffffff8); // media byte marker
        QCOMPARE(fatbuf[1], (uint32_t) 0x0fffffff); // end of chain marker
        QCOMPARE(fatbuf[2], (uint32_t) 4); // next cluster of dir 1
        QCOMPARE(fatbuf[3], (uint32_t) 0x0fffffff); // end of chain marker
        QCOMPARE(fatbuf[4], (uint32_t) 5); // next cluster of dir 1
        QCOMPARE(fatbuf[5], (uint32_t) 0x0fffffff); // end of chain marker
        VERIFY_ARRAY(fatbuf, 6, FAT_CLUSTER, (uint32_t) 0); // everything else 0
    }

    // Try allocating one filemap and check the result
    void test_one_filemap() {
        const int test_clusters = 17;
        const uint32_t expected_entry = FAT_ENTRIES - test_clusters;

        uint32_t clust_nr = fat_alloc_end(test_clusters);
        // Check that filemap was allocated at the end of the image
        QCOMPARE(clust_nr, expected_entry);

        fat_finalize(DATA_CLUSTERS);

        // Check that the last fat page shows the file
        uint32_t *buf = (uint32_t *) alloc_guarded(
                (test_clusters + 2) * sizeof(uint32_t));
        image_fill((char *) buf,
                FAT_START + (expected_entry - 1) * sizeof(uint32_t),
                (test_clusters + 2) * sizeof(uint32_t));
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
    }

    // Create an image with restricted free space
    void test_unusable_clusters() {
        fat_alloc_beginning(1);
        fat_alloc_beginning(1);
        fat_alloc_end(10);
        fat_alloc_end(10);
        const uint32_t allocated = 22;

        fat_finalize(DATA_CLUSTERS / 2);

        const uint32_t expect_free = DATA_CLUSTERS / 2;
        const uint32_t expect_bad = DATA_CLUSTERS - allocated - expect_free;

        // Load the whole FAT for analysis
        uint32_t *buf = (uint32_t *) alloc_guarded(
                FAT_ENTRIES * sizeof(uint32_t));
        image_fill((char *) buf, FAT_START, FAT_ENTRIES * sizeof(uint32_t));
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
        QCOMPARE(fat_extend_chain(0), (uint32_t) 0);
        QCOMPARE(fat_extend_chain(FAT_ENTRIES), (uint32_t) 0);
    }

    void test_cluster_pos() {
        uint64_t fat_end = FAT_START + ALIGN(FAT_ENTRIES * 4, SECTOR_SIZE);
        QCOMPARE(fat_cluster_pos(2), fat_end);
        QCOMPARE(fat_cluster_pos(3), fat_end + CLUSTER_SIZE);
    }
};

QTEST_APPLESS_MAIN(TestFat)
#include "tst_fat.moc"
