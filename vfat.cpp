/*
 * Copyright (C) 2013 Jolla Ltd.
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

#include "vfat.h"

#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <time.h>
#include <unistd.h>

#include <vector>

#define CLUSTER_SIZE 4096  /* must be a power of two */
#define SECTORS_PER_CLUSTER (CLUSTER_SIZE / SECTOR_SIZE)
#define RESERVED_SECTORS 32  /* before first FAT */

/*
 * The layout of a FAT filesystem is very simple.
 *
 * - First come RESERVED_SECTORS sectors, which include the boot sector
 * and the filesystem information sector.
 *
 * - Then comes a file allocation table (usually there are two copies
 * but only one is needed here because it's not a real on-disk filesystem).
 *
 * - Then come the data clusters, which are CLUSTER_SIZE each.
 *
 * - Everything is aligned on sector boundaries.
 *
 * - The FAT uses 4 bytes per data cluster to record allocation.
 * The allocations are singly linked lists, with each entry pointing
 * to the next or being an end marker. The first two entries are dummy
 * and don't refer to data clusters.
 *
 * - Some of the files recorded in the FAT are directories, which contain
 * lists of filenames, file sizes and their starting cluster numbers.
 * The boot sector has a pointer to the root directory.
 */

/* This is part of the FAT spec: a fatfs with less than this
 * number of clusters must be FAT12 or FAT16 */
#define MIN_FAT32_CLUSTERS 65525
/* Despite its name, FAT32 only uses 28 bits in its entries.
 * The top 4 bits should be cleared when allocating.
 * Entries 0x0ffffff0 and higher are reserved, as are 0 and 1. */
#define MAX_FAT32_CLUSTERS (0x0ffffff0 - 2)


/* The traditional definition of min, unsafe against side effects in a or b
 * but at least not limiting the types of a or b */
#define min(a, b) ((a) < (b) ? (a) : (b))

static const char *g_top_dir;
static uint64_t g_free_space;

static uint32_t g_fat_sectors;
static uint32_t g_data_clusters;
static uint32_t g_total_sectors;
/* strategy: lay out directories at the start of the fat, and
 * files at the end of the fat, with the free clusters between them.
 * note: these are cluster numbers as used in the FAT, so they are
 * offset by 2 compared to the actual data clusters
 */
static uint32_t g_first_free_cluster;
static uint32_t g_last_free_cluster;


/* All multibyte values in here are stored in little-endian format */
uint8_t boot_sector[SECTOR_SIZE] = {
	0xeb, 0x58, 0x90,  /* x86 asm, jump to offset 0x5a */
	'T', 'O', 'J', 'B', 'L', 'O', 'C', 'K', /* system id */
	/* 0x00B, start of bios parameter block */
	SECTOR_SIZE & 0xff, (SECTOR_SIZE >> 8) & 0xff,
	SECTORS_PER_CLUSTER,
	RESERVED_SECTORS & 0xff, (RESERVED_SECTORS >> 8) & 0xff,
	1, /* number of FATs */
	0, 0, /* root directory size, N/A for FAT32 */
	0, 0, /* number of sectors, stored below for FAT32 */
	0xf8, /* media descriptor: "fixed disk" */
	0, 0, /* sectors per FAT, stored below for FAT32 */
	1, 0, 1, 0,  /* cylinders and heads info, unused */
	0, 0, 0, 0,  /* sectors before start of partition */
#define SECTORCOUNT_OFFSET 0x20
	0, 0, 0, 0,  /* total sectors */
#define FATSECTORS_OFFSET 0x24
	0, 0, 0, 0,  /* sectors per FAT */
	0, 0,  /* flags about FAT usage, can be left 0 */
	0, 0,  /* fat32 format version 0.0 */
	2, 0, 0, 0,  /* cluster number of root directory */
	1, 0,  /* location of filesystem information sector */
	0, 0,  /* location of backup boot sector (none) */
	0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 12 bytes reserved */
	0x80,  /* drive number; 0x80 for first fixed disk */
	0,  /* reserved */
	0x29,  /* indicates next 3 fields are valid */
#define VOLUME_ID_OFFSET 0x43
	0, 0, 0, 0,  /* volume serial number, try to be unique */
	'T', 'O', 'J', 'B', 'L', 'O', 'C', 'K', 'F', 'S', ' ',  /* label */
	'F', 'A', 'T', '3', '2', ' ', ' ', ' ',  /* filesystem type */
	0, /* the rest is zero filled */
};
	
uint8_t fsinfo_sector[SECTOR_SIZE];

/* This holds the early part of the FAT: the initial 2 entries and the
 * allocated directory clusters. What follows is the free space and
 * the mapped files. Those are synthesized when queried, because they're
 * too repetitive to be worth storing directly. */
std::vector<uint32_t> fat_beginning;

/* Information about the data clusters allocated to directories.
 * Directories are allocated from the start of the FAT, but to make
 * the scanning code simpler they don't have to be allocated continuously
 * the way mapped files are. */
struct dir_cluster {
	uint32_t starting_cluster; /* number of first cluster in this dir */
	uint32_t cluster_offset;  /* number of preceding clusters */
	/* the following are only valid if cluster_offset == 0 */
	std::vector<char> data;
	const char *path;  /* path down from g_target_dir */
};

/* dir_clusters is indexed by cluster_nr - 2 */
std::vector<struct dir_cluster> dir_clusters;

/*
 * The FAT sectors are deduced from the stored file and directory info,
 * and are created on demand in this function.
 */
void fat_fill(void *vbuf, uint32_t entry_nr, uint32_t entries)
{
	uint32_t *buf = (uint32_t *)vbuf;
	uint32_t i = 0;

	if (entry_nr < fat_beginning.size()) {
		uint32_t count = min(entries, fat_beginning.size() - entry_nr);
		memcpy(vbuf, (void *) &fat_beginning[0], count * 4);
		i = count;
	}

	for (; i < entries; i++) {
		buf[i] = htole32(0);
	}
}

int vfat_fill(void *buf, uint64_t from, uint32_t len)
{
	while (len > 0) {
		uint32_t maxcopy = len;
		uint32_t sector_nr = from / SECTOR_SIZE;
		if (sector_nr < RESERVED_SECTORS) {
			uint32_t offset = from % SECTOR_SIZE;
			if (sector_nr == 0) {
				maxcopy = min(len, SECTOR_SIZE - from);
				memcpy(buf, &boot_sector[offset], maxcopy);
			} else if (sector_nr == 1) {
				maxcopy = min(len, 2*SECTOR_SIZE - from);
				memcpy(buf, &fsinfo_sector[offset], maxcopy);
			} else {
				maxcopy = min(len,
					RESERVED_SECTORS * SECTOR_SIZE - from);
				memset(buf, 0, maxcopy);
			}
		} else if (sector_nr < RESERVED_SECTORS + g_fat_sectors) {
			/* FAT sector */
			uint32_t entry_nr = (from
				- RESERVED_SECTORS * SECTOR_SIZE) / 4;
			maxcopy = min(len, (RESERVED_SECTORS + g_fat_sectors)
				* SECTOR_SIZE - from);
			if (from % 4 || maxcopy < 4) {
				/* deal with unaligned reads */
				uint32_t fat_entry;
				fat_fill(&fat_entry, entry_nr, 1);
				maxcopy = min(maxcopy, 4);
				memcpy(buf, ((char *)&fat_entry) + from % 4,
					maxcopy);
			} else {
				maxcopy -= maxcopy % 4;
				fat_fill(buf, entry_nr, maxcopy / 4);
			}
		} else if (sector_nr < g_total_sectors) {
			// uint32_t data_cluster = from / CLUSTER_SIZE;
			uint32_t offset = from % CLUSTER_SIZE;
			/* stub: data cluster */
			maxcopy = min(len, CLUSTER_SIZE - offset);
			memset(buf, 0, maxcopy);
		} else {
			/* past end of image */
			memset(buf, 0, len);
			return -EINVAL;
		}
		len -= maxcopy;
		buf = (char *) buf + maxcopy;
		from += maxcopy;
	}

	return 0;
}

void init_boot_sector(void)
{
	uint32_t volume_id = time(NULL);

	boot_sector[SECTORCOUNT_OFFSET + 0] = g_total_sectors;
	boot_sector[SECTORCOUNT_OFFSET + 1] = g_total_sectors >> 8;
	boot_sector[SECTORCOUNT_OFFSET + 2] = g_total_sectors >> 16;
	boot_sector[SECTORCOUNT_OFFSET + 3] = g_total_sectors >> 24;

	boot_sector[FATSECTORS_OFFSET + 0] = g_fat_sectors;
	boot_sector[FATSECTORS_OFFSET + 1] = g_fat_sectors >> 8;
	boot_sector[FATSECTORS_OFFSET + 2] = g_fat_sectors >> 16;
	boot_sector[FATSECTORS_OFFSET + 3] = g_fat_sectors >> 24;

	boot_sector[VOLUME_ID_OFFSET + 0] = volume_id;
	boot_sector[VOLUME_ID_OFFSET + 1] = volume_id >> 8;
	boot_sector[VOLUME_ID_OFFSET + 2] = volume_id >> 16;
	boot_sector[VOLUME_ID_OFFSET + 3] = volume_id >> 24;
}

void init_fsinfo_sector(void)
{
	/* Nothing really useful here, but it's expected to be present */
	memcpy(&fsinfo_sector[0], "RRaA", 4);  /* magic */
	memcpy(&fsinfo_sector[0x1e4], "rrAa", 4);  /* more magic */
	/* unset values for first free cluster and last allocated cluster */
	memcpy(&fsinfo_sector[0x1e8], "\xff\xff\xff\xff", 4);
	memcpy(&fsinfo_sector[0x1ec], "\xff\xff\xff\xff", 4);
	memcpy(&fsinfo_sector[0x1fc], "\0\0\x55\xaa", 4);  /* magic here too */
}

void vfat_init(const char *target_dir, uint64_t free_space)
{
	g_top_dir = target_dir;
	g_free_space = free_space;

	/* The FAT starts with two dummy entries */
	g_first_free_cluster = 2;
	g_last_free_cluster = g_data_clusters + 1;

	init_boot_sector();
	init_fsinfo_sector();

	/* entry 0 contains the media descriptor in its low byte,
	 * should be the same as in the boot sector. */
	fat_beginning.push_back(htole32(0x0ffffff8));
	/* entry 1 contains the end-of-chain marker */
	fat_beginning.push_back(htole32(0x0fffffff));
}

/* TODO: do something about the hidden coupling between this function
 * and vfat_init above. */
uint32_t vfat_adjust_size(uint32_t sectors, uint32_t sector_size)
{
	uint32_t data_clusters;
	uint32_t fat_sectors;
	if (sector_size != SECTOR_SIZE)
		return 0;

	/* first calculation is far too optimistic because we need fat space */
	data_clusters = (sectors - RESERVED_SECTORS) / SECTORS_PER_CLUSTER;
	fat_sectors = ALIGN((data_clusters + 2) * 4, SECTOR_SIZE);

	/* second calculation corrects for that */
	data_clusters = (sectors - fat_sectors - RESERVED_SECTORS)
		/ SECTORS_PER_CLUSTER;
	if (data_clusters < MIN_FAT32_CLUSTERS)  /* imposed by spec */
		data_clusters = MIN_FAT32_CLUSTERS;
	if (data_clusters > MAX_FAT32_CLUSTERS)
		data_clusters = MAX_FAT32_CLUSTERS;
	fat_sectors = ALIGN((data_clusters + 2) * 4, SECTOR_SIZE);

	g_fat_sectors = fat_sectors;
	g_data_clusters = data_clusters;
	g_total_sectors = RESERVED_SECTORS + fat_sectors
		+ data_clusters * SECTORS_PER_CLUSTER;
	return g_total_sectors;
}
