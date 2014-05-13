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

/*
 * This file is the interface for mapping local files into the FAT image.
 */

#include <stdint.h>

/* Call this after fat_init() */
void filemap_init();

/* Register a filemap and return its starting cluster number. */
uint32_t filemap_add(const char *name, uint32_t size);

/* Fill all or part of 'buf' with data from the mapped file,
 * starting from byte 'offset'. If not all of 'buf' is filled
 * (file is not long enough) then the rest is zeroed.
 * Result: 0 for success or errno for failure. */
int filemap_fill(char *buf, uint32_t len, int fmap_index, uint32_t offset);
