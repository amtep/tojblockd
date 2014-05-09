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

class TestFat : public QObject {
    Q_OBJECT

    static const int DATA_CLUSTERS = 1000000;

private slots:
    void init() {
        fat_init(DATA_CLUSTERS);
    }

    void test_empty_fat() {
        // No dirs should be found
        QCOMPARE(fat_dir_index(0), -1);
        QCOMPARE(fat_dir_index(1000), -1);

	fat_finalize(DATA_CLUSTERS); // required by API

	uint32_t buf[1024 + 1];
	buf[1024] = 0x31337; // buffer overflow guard value
	fat_fill(buf, 0, 1024);
	QCOMPARE(buf[1024], (uint32_t) 0x31337);
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

};

QTEST_APPLESS_MAIN(TestFat)
#include "tst_fat.moc"
