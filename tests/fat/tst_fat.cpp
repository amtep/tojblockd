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

private slots:


};

QTEST_APPLESS_MAIN(TestFat)
#include "tst_fat.moc"
