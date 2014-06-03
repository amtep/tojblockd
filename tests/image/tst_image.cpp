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

#include "image.h"

#include <errno.h>

#include <sys/mman.h>

#include <QtTest/QtTest>

#include "../helpers.h"

class TestDataService : public DataService {
public:
    TestDataService() : fill_errno(0), receive_errno(0), deleted(false) { }

    virtual int fill(char *buf, uint32_t length, uint64_t offset) {
        struct call_info info = { buf, length, offset };
        fill_calls.append(info);
        if (fill_errno == 0)
            memset(buf, 0, length);
        return fill_errno;
    }

    virtual int receive(const char *buf, uint32_t length, uint64_t offset) {
        struct call_info info = { buf, length, offset };
        receive_calls.append(info);
        return receive_errno;
    }

    virtual void final_deref() {
        Q_ASSERT(m_refs == 0);
        deleted = true;
    }

    struct call_info {
        const char *buf;
        uint32_t length;
        uint64_t offset;
    };

    QList<call_info> fill_calls;
    QList<call_info> receive_calls;
    int fill_errno;
    int receive_errno;
    bool deleted;
};

class TestImage : public QObject {
    Q_OBJECT

    static const int DATASIZE = 4096; // general default size for tests
    // data will be allocated and filled with non-zero values by alloc_data()
    // and it will be freed by cleanup()
    char *data;

private slots:
    void init() {
        data = 0;
        image_init();
    }

    void cleanup() {
        free_guarded(data);
    }

private:
    void alloc_data(size_t size) {
        free_guarded(data);
        data = (char *) alloc_guarded(size);
        memset(data, 1, size);
    }

    void alloc_ro_data(size_t size) {
        free_guarded(data);
        data = (char *) alloc_guarded(size);
        memset(data, 'x', size);
        mprotect(data, size, PROT_READ);
    }

    // Helper for registering a standard set of columns for test data
    void datacolumns_1() {
        // ask_start and ask_length are the parameters to image_fill
        QTest::addColumn<uint64_t>("ask_start");
        QTest::addColumn<uint32_t>("ask_length");
        // bufpos is the offset in the image_fill buffer where the service
        // was asked to start filling
        QTest::addColumn<size_t>("bufpos");
        // fill_length is the number of bytes the service was asked to fill
        QTest::addColumn<uint32_t>("fill_length");
        // offset is the offset in the service's logical range that
        // it was asked to fill from
        QTest::addColumn<uint64_t>("offset");
    }

    // Helper for adding a row defined by datacolumns_1
    // Its main benefit is the automatic type coercion of integers,
    // which saves a lot of noise in the callers.
    void addrow_1(const char *label, uint64_t ask_start, uint32_t ask_length,
            size_t bufpos, uint32_t fill_length, uint64_t offset) {
        QTest::newRow(label)
                << ask_start << ask_length << bufpos << fill_length << offset;
    }

    // Helper for registering a standard set of columns for test data,
    // suitable for a test with two services.
    void datacolumns_2() {
        QTest::addColumn<uint64_t>("ask_start");
        QTest::addColumn<uint32_t>("ask_length");
        QTest::addColumn<size_t>("bufpos1");
        QTest::addColumn<uint32_t>("fill_length1");
        QTest::addColumn<uint64_t>("offset1");
        QTest::addColumn<size_t>("bufpos2");
        QTest::addColumn<uint32_t>("fill_length2");
        QTest::addColumn<uint64_t>("offset2");
    }

    // Helper for adding a row defined by datacolumns_2
    void addrow_2(const char *label, uint64_t ask_start, uint32_t ask_length,
            size_t bufpos1, uint32_t fill_length1, uint64_t offset1,
            size_t bufpos2, uint32_t fill_length2, uint64_t offset2) {
        QTest::newRow(label)
                << ask_start << ask_length
                << bufpos1 << fill_length1 << offset1
                << bufpos2 << fill_length2 << offset2;
    }

private slots:
    // An uninitialized image should contain all zeroes
    void test_empty_fill() {
        alloc_data(DATASIZE);
        image_fill(data, 0, DATASIZE);
        VERIFY_ARRAY(data, 0, DATASIZE, (char) 0);

        // Test some random other chunk of it
        alloc_data(DATASIZE);
        image_fill(data, 31337, DATASIZE/2);
        VERIFY_ARRAY(data, 0, DATASIZE/2, (char) 0);
        VERIFY_ARRAY(data, DATASIZE/2, DATASIZE, (char) 1);
    }

    // Register a data service and check that image_fill uses it
    void test_register() {
        TestDataService service;
        image_register(&service, 1024, DATASIZE, 0);
        QCOMPARE(service.m_refs, 1);

        QFETCH(uint64_t, ask_start);
        QFETCH(uint32_t, ask_length);
        alloc_data(ask_length);
        int ret = image_fill(data, ask_start, ask_length);
        QCOMPARE(ret, 0);
        // All data must have been zeroed, either as uninitialized
        // space by image_fill or by calls to service->fill.
        VERIFY_ARRAY(data, 0, (int) ask_length, (char) 0);
        QCOMPARE(service.fill_calls.size(), 1);
        TestDataService::call_info info = service.fill_calls.takeFirst();
        QFETCH(size_t, bufpos);
        COMPARE_POINTERS(info.buf, data + bufpos);
        QTEST(info.length, "fill_length");
        QTEST(info.offset, "offset");
    }

    void test_register_data() {
        datacolumns_1();

        const uint64_t start = 1024; // service is registered here
        const uint32_t size = DATASIZE;
        addrow_1("overlap start of range",
                start - 1024, size,
                1024, size - 1024, 0);
        addrow_1("exact fill",
                start, size,
                0, size, 0);
        addrow_1("overlap end of range",
                start + 1024, size,
                0, size - 1024, 1024);
        addrow_1("large fill containing range",
                start - 1024, 2 * size,
                1024, size, 0);
    }

    // Register a data service at multiple locations
    void test_register_multipart() {
        TestDataService service;

        image_register(&service, 1024, DATASIZE, 0);
        image_register(&service, 10240, DATASIZE, DATASIZE);
        QCOMPARE(service.m_refs, 2);

        QFETCH(uint64_t, ask_start);
        QFETCH(uint32_t, ask_length);
        alloc_data(ask_length);
        int ret = image_fill(data, ask_start, ask_length);
        QCOMPARE(ret, 0);
        // All data must have been zeroed, either as uninitialized
        // space by image_fill or by calls to service->fill.
        VERIFY_ARRAY(data, 0, (int) ask_length, (char) 0);
        QCOMPARE(service.fill_calls.size(), 1);
        TestDataService::call_info info = service.fill_calls.takeFirst();
        QFETCH(size_t, bufpos);
        COMPARE_POINTERS(info.buf, data + bufpos);
        QTEST(info.length, "fill_length");
        QTEST(info.offset, "offset");
    }

    void test_register_multipart_data() {
        datacolumns_1();

        // The test method will register the same service at pos 1024
        // and pos 10240, the latter part with an offset of DATASIZE.
        // Tests for part 1 (same as in test_register_data)
        const uint32_t start = 1024; // service is registered here
        const uint32_t size = DATASIZE;
        addrow_1("overlap start of range",
                start - 1024, size,
                1024, size - 1024, 0);
        addrow_1("exact fill",
                start, size,
                0, size, 0);
        addrow_1("overlap end of range",
                start + 1024, size,
                0, size - 1024, 1024);
        addrow_1("large fill containing range",
                start - 1024, 2 * size,
                1024, size, 0);

        // Tests for part 2, same as for part 1 except for the different
        // starting position and the "offset" is "size" higher.
        const int start2 = 10240;
        addrow_1("overlap start of range",
                start2 - 1024, size,
                1024, size - 1024, size);
        addrow_1("exact fill",
                start2, size,
                0, size, size);
        addrow_1("overlap end of range",
                start2 + 1024, size,
                0, size - 1024, size + 1024);
        addrow_1("large fill containing range",
                start2 - 1024, 2 * size,
                1024, size, size);
    }

    // Register a data service at multiple locations and fill both at once
    void test_register_multipart_large_fill() {
        TestDataService service;

        image_register(&service, 1024, DATASIZE, 0);
        image_register(&service, 10240, DATASIZE, DATASIZE);
        QCOMPARE(service.m_refs, 2);

        uint32_t ask_length = 10240 + 2 * DATASIZE;
        alloc_data(ask_length);
        int ret = image_fill(data, 0, ask_length);
        QCOMPARE(ret, 0);
        VERIFY_ARRAY(data, 0, (int) ask_length, (char) 0);
        QCOMPARE(service.fill_calls.size(), 2);

        TestDataService::call_info info = service.fill_calls.takeFirst();
        COMPARE_POINTERS(info.buf, data + 1024);
        QCOMPARE(info.length, (uint32_t) DATASIZE);
        QCOMPARE(info.offset, (uint64_t) 0);

        info = service.fill_calls.takeFirst();
        COMPARE_POINTERS(info.buf, data + 10240);
        QCOMPARE(info.length, (uint32_t) DATASIZE);
        QCOMPARE(info.offset, (uint64_t) DATASIZE);
    }

    // Register two adjacent data services and do an image_fill
    // that hits both.
    void test_fill_adjacent_ranges() {
        TestDataService service1;
        TestDataService service2;
        TestDataService::call_info info;

        image_register(&service1, 1024, DATASIZE/2, 0);
        QCOMPARE(service1.m_refs, 1);
        image_register(&service2, 1024 + DATASIZE/2, DATASIZE/2, 0);
        QCOMPARE(service2.m_refs, 1);

        QFETCH(uint64_t, ask_start);
        QFETCH(uint32_t, ask_length);
        alloc_data(ask_length);
        int ret = image_fill(data, ask_start, ask_length);
        QCOMPARE(ret, 0);
        VERIFY_ARRAY(data, 0, (int) ask_length, (char) 0);
        QCOMPARE(service1.fill_calls.size(), 1);
        info = service1.fill_calls.takeFirst();
        QFETCH(size_t, bufpos1);
        COMPARE_POINTERS(info.buf, data + bufpos1);
        QTEST(info.length, "fill_length1");
        QTEST(info.offset, "offset1");

        QCOMPARE(service2.fill_calls.size(), 1);
        info = service2.fill_calls.takeFirst();
        QFETCH(size_t, bufpos2);
        COMPARE_POINTERS(info.buf, data + bufpos2);
        QTEST(info.length, "fill_length2");
        QTEST(info.offset, "offset2");
    }

    void test_fill_adjacent_ranges_data() {
        datacolumns_2();

        const uint32_t size = DATASIZE/2; // each range is this long
        const uint32_t start = 1024;

        addrow_2("overlap start of range",
                start - 1024, 2 * size,
                1024,        size,            0,
                1024 + size, DATASIZE - 1024 - size, 0);
        addrow_2("exact fill",
                start, 2 * size,
                0,    size, 0,
                size, size, 0);
        addrow_2("overlap end of range",
                start + 1024, 2 * size,
                0,           size - 1024, 1024,
                size - 1024, size,        0);
        addrow_2("overlap some of each",
                start + 1024, size,
                0,           size - 1024, 1024,
                size - 1024, 1024, 0);
        addrow_2("large fill containing both",
                start - 1024, 1024 + 2 * size + 1024,
                1024,        size, 0,
                1024 + size, size, 0);
    }

    // Register two almost adjacent data services and do an image_fill
    // that hits both.
    void test_fill_nearby_ranges() {
        TestDataService service1;
        TestDataService service2;
        TestDataService::call_info info;

        image_register(&service1, 1024, DATASIZE, 0);
        image_register(&service2, 1024 + DATASIZE + 100, DATASIZE, 0);

        QFETCH(uint64_t, ask_start);
        QFETCH(uint32_t, ask_length);
        alloc_data(ask_length);
        int ret = image_fill(data, ask_start, ask_length);
        QCOMPARE(ret, 0);
        VERIFY_ARRAY(data, 0, (int) ask_length, (char) 0);
        QCOMPARE(service1.fill_calls.size(), 1);
        info = service1.fill_calls.takeFirst();
        QFETCH(size_t, bufpos1);
        COMPARE_POINTERS(info.buf, data + bufpos1);
        QTEST(info.length, "fill_length1");
        QTEST(info.offset, "offset1");

        QCOMPARE(service2.fill_calls.size(), 1);
        info = service2.fill_calls.takeFirst();
        QFETCH(size_t, bufpos2);
        COMPARE_POINTERS(info.buf, data + bufpos2);
        QTEST(info.length, "fill_length2");
        QTEST(info.offset, "offset2");
    }

    void test_fill_nearby_ranges_data() {
        datacolumns_2();

        const uint32_t spacing = 100;
        const uint32_t start = 1024;
        const uint32_t size = DATASIZE;
        // delta is the difference between the start of the services
        const uint32_t delta = size + spacing;

        addrow_2("overlap start of range",
                start - 1024, 2 * size,
                1024, size, 0,
                1024 + delta, 2 * size - 1024 - delta, 0);
        addrow_2("exact fill",
                start, 2 * size + spacing,
                0, size, 0,
                delta, size, 0);
        addrow_2("overlap end of range",
                start + 1024, 2 * size,
                0, size - 1024, 1024,
                delta - 1024, size, 0);
        addrow_2("overlap some of each",
                start + 1024, size,
                0, size - 1024, 1024,
                delta - 1024, size - (delta - 1024), 0);
        addrow_2("large fill containing both",
                start - 1024, 1024 + size + spacing + size + 1024,
                1024, size, 0,
                1024 + delta, size, 0);
    }

    // Register a data service, then register another data service
    // partly overlapping it, and check that both are valid in their ranges.
    void test_overlapping_services() {
        TestDataService service1;
        TestDataService service2;
        TestDataService::call_info info;

        image_register(&service1, 1024, DATASIZE, 0);
        image_register(&service2, 1024 + DATASIZE/2, DATASIZE, 0);
        QCOMPARE(service1.m_refs, 1);
        QCOMPARE(service2.m_refs, 1);

        alloc_data(DATASIZE);
        int ret = image_fill(data, 1024, DATASIZE);
        QCOMPARE(ret, 0);
        VERIFY_ARRAY(data, 0, DATASIZE, (char) 0);
        QCOMPARE(service1.fill_calls.size(), 1);
        info = service1.fill_calls.takeFirst();
        COMPARE_POINTERS(info.buf, data);
        QCOMPARE(info.length, (uint32_t) DATASIZE/2);
        QCOMPARE(info.offset, (uint64_t) 0);

        QCOMPARE(service2.fill_calls.size(), 1);
        info = service2.fill_calls.takeFirst();
        COMPARE_POINTERS(info.buf, data + DATASIZE/2);
        QCOMPARE(info.length, (uint32_t) DATASIZE/2);
        QCOMPARE(info.offset, (uint64_t) 0);
    }

    // Register a data service, then register another data service
    // on top of it, and check that the first is dereferenced.
    void test_service_replace() {
        TestDataService service1;
        TestDataService service2;

        image_register(&service1, 1024, DATASIZE, 0);
        QCOMPARE(service1.m_refs, 1);
        image_register(&service2, 1024, DATASIZE, 0);
        QCOMPARE(service1.m_refs, 0);
        QCOMPARE(service2.m_refs, 1);
    }

    // Register a data service in two places, then register another
    // service on top of one of the places, then check that the first
    // still has a reference open.
    void test_service_replace_instance() {
        TestDataService service1;
        TestDataService service2;

        image_register(&service1, 1024, DATASIZE, 0);
        image_register(&service1, 10240, DATASIZE, DATASIZE);
        image_register(&service2, 1024, DATASIZE, 0);
        QCOMPARE(service1.m_refs, 1);
        QCOMPARE(service2.m_refs, 1);
    }

    // Register a service over a >4GB range, and check image_fill on
    // parts of it.
    void test_huge_service() {
        const uint64_t length = 50ul * 1024 * 1024 * 1024; // 50 GiB
        const uint64_t start = 10ul * 1024 * 1024 * 1024; // 10 GiB
        TestDataService service;

        image_register(&service, start, length, 0);
        QFETCH(uint64_t, ask_start);
        QFETCH(uint32_t, ask_length);
        alloc_data(ask_length);
        int ret = image_fill(data, ask_start, ask_length);
        QCOMPARE(ret, 0);
        // All data must have been zeroed, either as uninitialized
        // space by image_fill or by calls to service->fill.
        VERIFY_ARRAY(data, 0, (int) ask_length, (char) 0);
        QCOMPARE(service.fill_calls.size(), 1);
        TestDataService::call_info info = service.fill_calls.takeFirst();
        QFETCH(size_t, bufpos);
        COMPARE_POINTERS(info.buf, data + bufpos);
        QTEST(info.length, "fill_length");
        QTEST(info.offset, "offset");
    }

    void test_huge_service_data() {
        const uint64_t length = 50ul * 1024 * 1024 * 1024; // 50 GiB
        const uint64_t start = 10ul * 1024 * 1024 * 1024; // 10 GiB

        datacolumns_1();

        addrow_1("overlap start of range",
                start - 1024, DATASIZE,
                1024, DATASIZE - 1024, 0);
        addrow_1("overlap end of range",
                start + length - 1024, DATASIZE,
                0, 1024, length - 1024);
        addrow_1("somewhere in the middle",
                start + length/2, DATASIZE * 1024,
                0, DATASIZE * 1024, length/2);
    }

    // Call image_receive on an undefined part of the image
    void test_image_receive_unregistered() {
        // The image should just store the received data and return it on fills
        alloc_ro_data(DATASIZE);
        int ret = image_receive(data, 1000, DATASIZE);
        QCOMPARE(ret, 0);

        alloc_data(DATASIZE);
        ret = image_fill(data, 1000, DATASIZE);
        QCOMPARE(ret, 0);
        VERIFY_ARRAY(data, 0, DATASIZE, 'x'); // 'x' is from alloc_ro_data
    }

    // Register a data service and then an image_receive on part of its range.
    // Check that image_fill returns data from the service in its remaining
    // range, and data from the image_receive in its range.
    void test_image_receive_partial_overlap() {
        const uint32_t delta = 1024;
        TestDataService::call_info info;
        TestDataService service;
        image_register(&service, 1024, DATASIZE, 0);

        alloc_ro_data(DATASIZE);
        int ret = image_receive(data, 1024 + delta, DATASIZE);
        QCOMPARE(ret, 0);
        QCOMPARE(service.m_refs, 1);
        QCOMPARE(service.receive_calls.size(), 1);
        info = service.receive_calls.takeFirst();
        COMPARE_POINTERS(info.buf, data);
        QCOMPARE(info.length, DATASIZE - delta);
        QCOMPARE(info.offset, (uint64_t) delta);

        alloc_data(DATASIZE + delta);
        ret = image_fill(data, 1024, DATASIZE + delta);
        QCOMPARE(ret, 0);
        VERIFY_ARRAY(data, 0, (int) delta, (char) 0);
        VERIFY_ARRAY(data, (int) delta, DATASIZE + (int) delta, 'x');
    }

    // Register two almost adjacent data services and do an image_receive
    // that overlaps both.
    void test_image_receive_multi_service() {
        const uint32_t spacing = 100;
        TestDataService::call_info info;
        TestDataService service1;
        TestDataService service2;

        image_register(&service1, 1024, DATASIZE, 0);
        image_register(&service2, 1024 + DATASIZE + spacing, DATASIZE, 0);

        alloc_ro_data(2 * DATASIZE);
        int ret = image_receive(data, 1024, 2 * DATASIZE);
        QCOMPARE(ret, 0);
        QCOMPARE(service1.m_refs, 1);
        QCOMPARE(service2.m_refs, 1);

        QCOMPARE(service1.receive_calls.size(), 1);
        info = service1.receive_calls.takeFirst();
        COMPARE_POINTERS(info.buf, data);
        QCOMPARE(info.length, (uint32_t) DATASIZE);
        QCOMPARE(info.offset, (uint64_t) 0);

        QCOMPARE(service2.receive_calls.size(), 1);
        info = service2.receive_calls.takeFirst();
        COMPARE_POINTERS(info.buf, data + DATASIZE + spacing);
        QCOMPARE(info.length, DATASIZE - spacing);
        QCOMPARE(info.offset, (uint64_t) 0);
    }

    // Register two data services and do an image_receive that overlaps
    // both, and check response to errors from service->receive.
    void test_image_receive_errors() {
        TestDataService::call_info info;
        TestDataService service1;
        TestDataService service2;

        image_register(&service1, 1024, DATASIZE, 0);
        image_register(&service2, 1024 + DATASIZE, DATASIZE, 0);
        service1.receive_errno = EIO;

        alloc_ro_data(2 * DATASIZE);
        int ret = image_receive(data, 1024, 2 * DATASIZE);
        QCOMPARE(ret, EIO);
        QCOMPARE(service1.receive_calls.size(), 1);
        QCOMPARE(service2.receive_calls.size(), 0);

        // Check that the received data was not stored

        alloc_data(2 * DATASIZE);
        ret = image_fill(data, 1024, 2 * DATASIZE);
        QCOMPARE(ret, 0);
        VERIFY_ARRAY(data, 0, 2 * DATASIZE, (char) 0);

        QCOMPARE(service1.fill_calls.size(), 1);
        info = service1.fill_calls.takeFirst();
        COMPARE_POINTERS(info.buf, data);
        QCOMPARE(info.length, (uint32_t) DATASIZE);
        QCOMPARE(info.offset, (uint64_t) 0);

        QCOMPARE(service2.fill_calls.size(), 1);
        info = service2.fill_calls.takeFirst();
        COMPARE_POINTERS(info.buf, data + DATASIZE);
        QCOMPARE(info.length, (uint32_t) DATASIZE);
        QCOMPARE(info.offset, (uint64_t) 0);
    }

    // Register a data service and clear it, check that it gets dereffed,
    // check that the range now returns zeroes without calling the service
    void test_clear_service() {
        TestDataService service;

        image_register(&service, 1024, DATASIZE, 0);
        QCOMPARE(service.m_refs, 1);
        image_clear_services(1024, DATASIZE);
        QCOMPARE(service.m_refs, 0);

        alloc_data(DATASIZE);
        int ret = image_fill(data, 1024, DATASIZE);
        QCOMPARE(ret, 0);
        VERIFY_ARRAY(data, 0, DATASIZE, (char) 0);
        QCOMPARE(service.fill_calls.size(), 0);
    }

    // Do an image_receive on an undefined part of the image,
    // then image_clear_data and check that it now returns zeroes
    void test_clear_received() {
        alloc_ro_data(DATASIZE);
        int ret = image_receive(data, 1024, DATASIZE);
        QCOMPARE(ret, 0);

        image_clear_data(1024 + DATASIZE/2, DATASIZE);

        alloc_data(DATASIZE);
        ret = image_fill(data, 1024, DATASIZE);
        QCOMPARE(ret, 0);
        VERIFY_ARRAY(data, 0, DATASIZE/2, 'x');
        VERIFY_ARRAY(data, DATASIZE/2, DATASIZE, (char) 0);
    }

    // Register a data service in two locations and clear one of them,
    // check it keeps at least one ref, check it still works
    void test_clear_service_multi_offset() {
        TestDataService::call_info info;
        TestDataService service;

        image_register(&service, 1024, DATASIZE, 0);
        image_register(&service, 10240, DATASIZE, DATASIZE);
        QCOMPARE(service.m_refs, 2);
        image_clear_services(1024, DATASIZE);
        QCOMPARE(service.m_refs, 1);

        alloc_data(DATASIZE);
        int ret = image_fill(data, 10240, DATASIZE);
        QCOMPARE(ret, 0);
        VERIFY_ARRAY(data, 0, DATASIZE, (char) 0);
        QCOMPARE(service.fill_calls.size(), 1);
        info = service.fill_calls.takeFirst();
        COMPARE_POINTERS(info.buf, data);
        QCOMPARE(info.length, (uint32_t) DATASIZE);
        QCOMPARE(info.offset, (uint64_t) DATASIZE);
    }

    // Register a data service and clear part of it, check that the
    // remaining part still works and it wasn't dereffed
    void test_clear_service_partial() {
        TestDataService::call_info info;
        TestDataService service;

        image_register(&service, 1024, DATASIZE, 0);
        QCOMPARE(service.m_refs, 1);
        image_clear_services(1024 + DATASIZE/2, DATASIZE);
        QCOMPARE(service.m_refs, 1);

        alloc_data(DATASIZE);
        int ret = image_fill(data, 1024, DATASIZE);
        QCOMPARE(ret, 0);
        VERIFY_ARRAY(data, 0, DATASIZE, (char) 0);
        QCOMPARE(service.fill_calls.size(), 1);
        info = service.fill_calls.takeFirst();
        COMPARE_POINTERS(info.buf, data);
        QCOMPARE(info.length, (uint32_t) DATASIZE/2);
        QCOMPARE(info.offset, (uint64_t) 0);
    }

    // Register a data service, then do an image_receive on top of
    // it, then an image_clear_services on that range. Check that image_fill
    // still returns the received data
    void test_clear_service_leaves_received() {
        TestDataService service;

        image_register(&service, 1024, DATASIZE, 0);

        alloc_ro_data(DATASIZE);
        int ret = image_receive(data, 1024, DATASIZE);
        QCOMPARE(ret, 0);

        image_clear_services(1024, DATASIZE);
        QCOMPARE(service.m_refs, 0);

        alloc_data(DATASIZE);
        ret = image_fill(data, 1024, DATASIZE);
        QCOMPARE(ret, 0);
        VERIFY_ARRAY(data, 0, DATASIZE, 'x'); // 'x' is from image_receive
        QCOMPARE(service.fill_calls.size(), 0);
    }

    // Register a data service, then do an image_receive on top of
    // it, then an image_clear_data on that range. Check that image_fill
    // now uses the service.
    void test_clear_data_leaves_service() {
        TestDataService::call_info info;
        TestDataService service;

        image_register(&service, 1024, DATASIZE, 0);

        alloc_ro_data(DATASIZE);
        int ret = image_receive(data, 1024, DATASIZE);
        QCOMPARE(ret, 0);

        image_clear_data(1024, DATASIZE);
        QCOMPARE(service.m_refs, 1);

        alloc_data(DATASIZE);
        ret = image_fill(data, 1024, DATASIZE);
        QCOMPARE(ret, 0);
        VERIFY_ARRAY(data, 0, DATASIZE, (char) 0);
        QCOMPARE(service.fill_calls.size(), 1);
        info = service.fill_calls.takeFirst();
        COMPARE_POINTERS(info.buf, data);
        QCOMPARE(info.length, (uint32_t) DATASIZE);
        QCOMPARE(info.offset, (uint64_t) 0);
    }

    // Register a data range of length 0, check that the module does not
    // retain a ref.
    void test_register_length0() {
        TestDataService service;

        image_register(&service, 5000, 0, 0);
        QCOMPARE(service.m_refs, 0);
        QVERIFY(service.deleted);

        alloc_data(DATASIZE);
        int ret = image_fill(data, 4000, DATASIZE);
        QCOMPARE(ret, 0);
        VERIFY_ARRAY(data, 0, DATASIZE, (char) 0);
        QCOMPARE(service.fill_calls.size(), 0);
    }
};

const int TestImage::DATASIZE;

QTEST_APPLESS_MAIN(TestImage)
#include "tst_image.moc"
