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

#include <QtTest/QtTest>

#include "image.h"

/*
 * Most of these measurements are done with one thousand operations
 * and again with one hundred thousand operations, rather than
 * relying on QBENCHMARK to decide on its own. The reason is to
 * check that the cost per operation is approximately linear.
 *
 * One hundred thousand is the target for the number of user files
 * to support.
 */

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
        // Since fat_extend_chain is mainly used for growing directories,
        // the amounts are scaled for a hundred thousand files at
        // approximately 50 file entries per directory cluster.
        QTest::addColumn<int>("count");
        QTest::newRow("20") << 200;
        QTest::newRow("2000") << 2000;
    }

    void test_extend_two() {
        QFETCH(int, count);
        uint32_t clust1 = fat_alloc_beginning(1);
        uint32_t clust2 = fat_alloc_beginning(1);
        QBENCHMARK {
            for (int i = 0; i < count; i++) {
                clust1 = fat_extend_chain(clust1);
                clust2 = fat_extend_chain(clust2);
            }
            fat_finalize(DATA_CLUSTERS);
        }
    }

    void test_extend_two_data() {
        test_extend_data();
    }
};

QTEST_APPLESS_MAIN(BenchFat)
#include "bench_fat.moc"
