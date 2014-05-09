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

#include <QtTest/QtTest>

// Mock function
int filemap_fill(char *buf, uint32_t len, int, uint32_t)
{
    memset(buf, '5', len);
    return 0;
}

// Mock function
int dir_fill(char *buf, uint32_t len, int, uint32_t)
{
    memset(buf, 'A', len);
    return 0;
}

// Helper macro. Has to be a macro because QCOMPARE can only be
// used from the test function itself.
#define call_fat_fill(buf, entry_nr, entries) \
    uint32_t buf[(entries) + 1]; \
    buf[entries] = 0x31337; \
    fat_fill(buf, entry_nr, entries); \
    QCOMPARE(buf[entries], (uint32_t) 0x31337);

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
        for (int i = 2; i < 1024; i++) {
        	QCOMPARE(buf[i], (uint32_t) 0);
        }

        char data[4096 + 1];
        data[4096] = 'X'; // buffer overflow guard
        uint32_t filled;
        int ret = data_fill(data, 4096, 0, 0, &filled);
        QCOMPARE(data[4096], 'X');
        QCOMPARE(ret, 0);
        QCOMPARE(filled, (uint32_t) 4096);
        // everything should be 0
        for (int i = 0; i < 4096; i++) {
        	QCOMPARE(data[i], (char) 0);
        }
    }

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

};

QTEST_APPLESS_MAIN(TestFat)
#include "tst_fat.moc"
