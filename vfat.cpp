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

#define FAT_END_OF_CHAIN 0x0fffffff


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
	0xeb, 0xfe, 0x90,  /* x86 asm, infinite loop */
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

/* Filenames on the FAT side are represented by little-endian UTF-16 strings,
 * with a terminating 0 value which is included. */
typedef std::vector<uint16_t> filename_t;
#define DIR_ENTRY_SIZE 32
#define CHARS_PER_DIR_ENTRY 13

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
	uint32_t last_cluster;
};

/* dir_clusters is indexed by cluster_nr - 2 */
std::vector<struct dir_cluster> dir_clusters;

int extend_dir(uint32_t clust_nr, uint32_t extra_size)
{
	int clusters_needed;
	struct dir_cluster *dir;
	struct dir_cluster *chunk;

	if (clust_nr > dir_clusters.size() + 2)
		return -1;
	dir = &dir_clusters[clust_nr - 2];

	clusters_needed = ALIGN(dir->data.size() + extra_size, CLUSTER_SIZE)
		/ CLUSTER_SIZE;
	chunk = &dir_clusters[dir->last_cluster - 2];
	while (clusters_needed > chunk->cluster_offset + 1) {
		struct dir_cluster new_cluster;
		uint32_t new_cluster_nr = fat_beginning.size();

		new_cluster.starting_cluster = chunk->starting_cluster;
		new_cluster.cluster_offset = chunk->cluster_offset + 1;
		new_cluster.path = 0;
		dir_clusters.push_back(new_cluster);
		fat_beginning.push_back(htole32(FAT_END_OF_CHAIN));
		fat_beginning[dir->last_cluster] = htole32(new_cluster_nr);
		dir->last_cluster = new_cluster_nr;
		g_first_free_cluster++;  // TODO: what if there's no more space?
		chunk = &dir_clusters[dir->last_cluster - 2];
	}

	return 0;
}

void fill_filename_part(char *data, int seq_nr, bool is_last,
	const filename_t &filename, uint8_t checksum)
{
	static const int char_offsets[CHARS_PER_DIR_ENTRY] =
		{ 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
	int i;
	int fn_offset;
	const uint16_t *fn_data;
	int max_i;

	if (is_last)
		data[0] = seq_nr | 0x40;
	else
		data[0] = seq_nr;
	data[11] = 0x0f;  /* attributes: vfat entry */
	data[12] = 0;  /* reserved */
	data[13] = checksum;
	data[26] = 0;  /* cluster nr (unused) */
	data[27] = 0;  /* cluster nr (unused) */

	/* TODO: this loop could be speeded up by special-casing is_last,
	 * because that's the only time that the upper bound might change */

	fn_offset = (seq_nr - 1) * CHARS_PER_DIR_ENTRY;
	fn_data = &filename[0];
	max_i = min(CHARS_PER_DIR_ENTRY, filename.size() - fn_offset);
	for (i = 0; i < max_i; i++, fn_offset++) {
		data[char_offsets[i]] = fn_data[fn_offset] & 0xff;
		data[char_offsets[i] + 1] = fn_data[fn_offset] >> 8;
	}
	for ( ; i < CHARS_PER_DIR_ENTRY; i++) {
		data[char_offsets[i]] = 0xff;
		data[char_offsets[i] + 1] = 0xff;
	}
}

uint8_t calc_vfat_checksum(uint8_t *entry)
{
	uint8_t sum = 0;
	int i;

	for (i = 0; i < 11; i++) {
		sum = ((sum & 1) << 7) + (sum >> 1) + entry[i];
	}
	return sum;
}

/*
 * Fill in just enough of the short entry to be able to calculate the checksum
 */
void prep_short_entry(uint8_t *entry)  /* at least 11-byte buffer */
{
	static uint32_t counter = 1;
	uint32_t uniq = counter++;
	int i;

	/* The first 11 bytes are the shortname buffer.
	 * Fill it with an invalid but still unique value.
	 * See http://lkml.org/lkml/2009/6/26/313 for the algorithm.
	 */

	entry[0] = ' ';
	entry[1] = 0;
	for (i = 2; i < 8; i++) {
		entry[i] = uniq & 0x1f;
		uniq >>= 5;
	}
	entry[8] = '/';
	entry[9] = 0;
	entry[10] = 0;
}

void encode_datetime(uint8_t *buf, time_t stamp)  /* 4-byte buffer */
{
	struct tm *t = gmtime(&stamp);
	uint16_t time_part;
	uint16_t date_part;

	time_part = (t->tm_sec / 2) | (t->tm_min << 5) | (t->tm_hour << 11);
	/* struct tm measures years from 1900, but FAT measures from 1980 */
	date_part = (t->tm_mday) | ((t->tm_mon + 1) << 5)
		| ((t->tm_year - 80) << 9);

	buf[0] = time_part & 0xff;
	buf[1] = (time_part >> 8) & 0xff;
	buf[2] = date_part & 0xff;
	buf[3] = (date_part >> 8) & 0xff;
}

int add_dir_entry(uint32_t parent_clust, uint32_t entry_clust,
	const filename_t &filename, uint32_t file_size, bool is_dir,
	time_t mtime)
{
	struct dir_cluster *parent;
	int num_entries;
	int ret = 0;
	int seq_nr;
	int data_offset;
	uint8_t checksum;
	uint8_t short_entry[DIR_ENTRY_SIZE];
	uint8_t attrs;

	if (parent_clust > dir_clusters.size() + 2)
		return -1;
	parent = &dir_clusters[parent_clust - 2];

	/* Check if the result will fit in the allocated space */
	/* add one entry for the shortname */
	num_entries = 1 + (filename.size() + CHARS_PER_DIR_ENTRY - 1)
				/ CHARS_PER_DIR_ENTRY;
	if (num_entries > 32) /* filesystem spec limitation */
		return -1;
	ret = extend_dir(parent_clust, num_entries * DIR_ENTRY_SIZE);
	if (ret)
		return ret;

	prep_short_entry(short_entry);
	attrs = 0x01;  /* always read-only */
	if (is_dir) {
		attrs |= 0x10;
		file_size = 0;
	}
	short_entry[11] = attrs;
	short_entry[12] = 0;
	/* Slightly higher resolution creation time.
	 * The normal time format only encodes down to 2-second precision. */
	short_entry[13] = (mtime & 1) * 100;
	/* this field calls for creation time but we don't have that, so
	 * substitute last modification time */
	encode_datetime(&short_entry[14], mtime);  /* 4 bytes */
	/* last access date, which is optional */
	short_entry[18] = 0;
	short_entry[19] = 0;
	short_entry[20] = (entry_clust >> 16) & 0xff;
	short_entry[21] = (entry_clust >> 24) & 0xff;
	encode_datetime(&short_entry[22], mtime);
	short_entry[26] = entry_clust & 0xff;
	short_entry[27] = (entry_clust >> 8) & 0xff;
	short_entry[28] = file_size & 0xff;
	short_entry[29] = (file_size >> 8) & 0xff;
	short_entry[30] = (file_size >> 16) & 0xff;
	short_entry[31] = (file_size >> 24) & 0xff;

	data_offset = parent->data.size();
	parent->data.resize(parent->data.size() + num_entries * DIR_ENTRY_SIZE);

	checksum = calc_vfat_checksum(short_entry);
	/* The name parts are stored last-to-first, with decreasing seq_nr */
	for (seq_nr = num_entries - 1; seq_nr >= 1; seq_nr--) {
		fill_filename_part(&parent->data[data_offset], seq_nr,
			seq_nr == num_entries - 1, filename, checksum);
		data_offset += 32;
	}

	memcpy(&parent->data[data_offset], short_entry, DIR_ENTRY_SIZE);
	return 0;
}

uint32_t alloc_new_dir(const char *path)
{
	struct dir_cluster new_cluster;

	new_cluster.starting_cluster = fat_beginning.size();
	new_cluster.last_cluster = new_cluster.starting_cluster;
	new_cluster.cluster_offset = 0;
	new_cluster.path = strdup(path);

	fat_beginning.push_back(htole32(FAT_END_OF_CHAIN));
	dir_clusters.push_back(new_cluster);
	g_first_free_cluster++;  // TODO: what if there's no more space?
	return new_cluster.starting_cluster;
}

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

void dir_fill(void *vbuf, uint32_t clust_nr, uint32_t offset, uint32_t maxcopy)
{
	struct dir_cluster *dir;
	char *buf = (char *) vbuf;
	uint32_t src_offset;

	dir = &dir_clusters[clust_nr - 2];
	src_offset = dir->cluster_offset * CLUSTER_SIZE + offset;
	if (dir->starting_cluster != clust_nr)
		dir = &dir_clusters[dir->starting_cluster - 2];
	if (src_offset + maxcopy > dir->data.size()) {
		uint32_t extra = src_offset + maxcopy - dir->data.size();
		if (extra > maxcopy)
			extra = maxcopy;
		maxcopy -= extra;
		memset(buf + maxcopy, 0, extra);
	}

	if (maxcopy)
		memcpy(buf, &dir->data[0] + src_offset, maxcopy);
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
			uint32_t adj = from - (RESERVED_SECTORS + g_fat_sectors)
				* SECTOR_SIZE;
			uint32_t data_cluster = (adj / CLUSTER_SIZE) + 2;
			uint32_t offset = adj % CLUSTER_SIZE;
			maxcopy = min(len, CLUSTER_SIZE - offset);
			if (data_cluster < fat_beginning.size()) {
				dir_fill(buf, data_cluster, offset, maxcopy);
			} else {
				memset(buf, 0, maxcopy);
			}
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
	fat_beginning.push_back(htole32(FAT_END_OF_CHAIN));

	alloc_new_dir("."); /* create empty root directory */

	/* for testing */
	int t_clust = alloc_new_dir("ts");
	filename_t dot_name;
	dot_name.push_back(htole16('.'));
	dot_name.push_back(0);
	add_dir_entry(t_clust, t_clust, dot_name, 0, true, time(NULL));
	dot_name[1] = htole16('.');
	dot_name.push_back(0);
	add_dir_entry(t_clust, 0, dot_name, 0, true, time(NULL));
	dot_name[0] = htole16('t');
	dot_name[1] = htole16('s');
	add_dir_entry(2, t_clust, dot_name, 0, true, time(NULL));
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
	fat_sectors = ALIGN((data_clusters + 2) * 4, SECTOR_SIZE) / SECTOR_SIZE;

	/* second calculation corrects for that */
	data_clusters = (sectors - fat_sectors - RESERVED_SECTORS)
		/ SECTORS_PER_CLUSTER;
	if (data_clusters < MIN_FAT32_CLUSTERS)  /* imposed by spec */
		data_clusters = MIN_FAT32_CLUSTERS;
	if (data_clusters > MAX_FAT32_CLUSTERS)
		data_clusters = MAX_FAT32_CLUSTERS;
	fat_sectors = ALIGN((data_clusters + 2) * 4, SECTOR_SIZE) / SECTOR_SIZE;

	g_fat_sectors = fat_sectors;
	g_data_clusters = data_clusters;
	g_total_sectors = RESERVED_SECTORS + fat_sectors
		+ data_clusters * SECTORS_PER_CLUSTER;
	return g_total_sectors;
}
