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

#include <errno.h>

#include <sys/mman.h>

#include <QtTest/QtTest>

#include "image.h"

class BenchFat : public QObject {
    Q_OBJECT

    static const uint32_t DATA_CLUSTERS = 1000000;

private slots:
    void init() {
        image_init();
        fat_init(DATA_CLUSTERS);
    }

    void test_alloc_end() {
        QFETCH(int, count);
        QFETCH(uint32_t, clusters);
        QBENCHMARK {
            for (int i = 0; i < count; i++) {
                fat_alloc_end(clusters);
            }
            fat_finalize(DATA_CLUSTERS);
        }
    }

    void test_alloc_end_data() {
        QTest::addColumn<int>("count");
        QTest::addColumn<uint32_t>("clusters");
        QTest::newRow("1k files") << 1000 << (uint32_t) 1;
        QTest::newRow("100k files") << 100000 << (uint32_t) 1;
        QTest::newRow("100k large files") << 100000 << (uint32_t) 1000;
    }

    void test_alloc_beginning() {
        QFETCH(int, count);
        QFETCH(uint32_t, clusters);
        QBENCHMARK {
            for (int i = 0; i < count; i++) {
                fat_alloc_beginning(clusters);
            }
            fat_finalize(DATA_CLUSTERS);
        }
    }

    void test_alloc_beginning_data() {
        test_alloc_end_data();
    }

    void test_extend() {
        QFETCH(int, count);
        uint32_t clust = fat_alloc_beginning(1);
        QBENCHMARK {
            for (int i = 0; i < count; i++) {
                clust = fat_extend_chain(clust);
            }
            fat_finalize(DATA_CLUSTERS);
        }
    }

    void test_extend_data() {
        QTest::addColumn<int>("count");
        QTest::newRow("1k") << 1000;
        QTest::newRow("100k") << 100000;
    }
};

QTEST_APPLESS_MAIN(BenchFat)
#include "bench_fat.moc"
