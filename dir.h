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

#include <stdint.h>

#include <time.h>

#include <vector>

/*
 * This file is the interface to FAT32 directory handling,
 * including directory entry creation and parsing.
 */

/* The attribute flags used in directory entries */
#define FAT_ATTR_NONE      0x00
#define FAT_ATTR_READ_ONLY 0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_LABEL     0x08
#define FAT_ATTR_DIRECTORY 0x10

#define FAT_ATTR_LFN       0x0f  /* marker for long file name segments */

#define ROOT_DIR_CLUSTER 2

/* Filenames are represented by little-endian UTF-16 strings,
 * with a terminating 0 value which is included. */
typedef std::vector<uint16_t> filename_t;

/* Call this after fat_init() to create the root directory */
void dir_init();

/* Extend the dir at parent_clust to include the new entry described
 * by the other parameters. Return true for success. */
bool dir_add_entry(uint32_t parent_clust, uint32_t entry_clust,
	const filename_t &filename, uint32_t file_size, uint8_t attrs,
	time_t mtime, time_t atime);

/* Register a new directory and return its starting cluster number */
uint32_t dir_alloc_new(const char *path);
