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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fts.h>

#include <vector>
#include "ConvertUTF.h"

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

/*
 * The virtual FAT image created here is always laid out with the
 * directories in the beginning and the files at the end, and all
 * the free space in between.
 */

/* This is part of the FAT spec: a fatfs with less than this
 * number of clusters must be FAT12 or FAT16 */
#define MIN_FAT32_CLUSTERS 65525
/* Despite its name, FAT32 only uses 28 bits in its entries.
 * The top 4 bits should be cleared when allocating.
 * Entries 0x0ffffff0 and higher are reserved, as are 0 and 1. */
#define MAX_FAT32_CLUSTERS (0x0ffffff0 - 2)

#define FAT_END_OF_CHAIN 0x0fffffff
#define FAT_BAD_CLUSTER  0x0ffffff7


/* The traditional definition of min, unsafe against side effects in a or b
 * but at least not limiting the types of a or b */
#define min(a, b) ((a) < (b) ? (a) : (b))

static const char *g_top_dir;

static uint32_t g_fat_sectors;
static uint32_t g_data_clusters;
static uint32_t g_total_sectors;
static uint32_t g_max_free_clusters;

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

/* Filenames on the FAT side are represented by little-endian UTF-16 strings,
 * with a terminating 0 value which is included. */
typedef std::vector<uint16_t> filename_t;
#define DIR_ENTRY_SIZE 32
#define CHARS_PER_DIR_ENTRY 13

// These are initialized by vfat_init
static filename_t dot_name;  // contains "."
static filename_t dot_dot_name;  // contains ".."

/*
 * Information about allocated directories.
 * Directories are allocated from the start of the FAT, but to make
 * the scanning code simpler they don't have to be allocated contiguously
 * the way mapped files are.
 * The fat_extents entries (defined below) point to these.
 */
struct dir_info {
	uint32_t starting_cluster; /* number of first cluster of this dir */
	/* last_extent is redundant with the information in fat_extents,
	 * but having it here means not having to do a linear scan. */
	uint32_t last_extent; /* extent containing last cluster of dir */
	std::vector<char> data;
	const char *path;  /* path down from g_top_dir */
};

std::vector<struct dir_info> dir_infos;

/*
 * Files are laid out without fragmentation in our FAT, so the only
 * cluster info needed is the starting cluster and the number of
 * clusters.
 */
struct filemap_info {
	uint32_t starting_cluster;
	uint32_t ending_cluster;
	const char *path;  /* path down from g_top_dir */
};

/* filemaps are kept sorted by descending starting_cluster */
std::vector<struct filemap_info> filemaps;

/*
 * A fat_extent is a contiguous section of the FAT where the values
 * are either all identical (empty, bad sector, etc) or are ascending
 * numbers where each value points to its neighbor, with possibly
 * an end-of-chain marker at the end.
 */
struct fat_extent {
	/* TODO: if the fat_extents cover the whole image then one
	 * of these is redundant. */
	uint32_t starting_cluster;
	uint32_t ending_cluster;
	uint32_t index;  /* index to file or dir table, or literal value */
	uint32_t offset; /* byte offset of starting_cluster in file or dir */
	uint8_t extent_type; /* EXTENT_ values */
	uint8_t terminated; /* boolean: is ending_cluster an end-of-chain? */
};

enum {
	EXTENT_LITERAL = 0, /* "index" is literal value */
	EXTENT_DIR = 1, /* index is into dir_infos */
	EXTENT_FILEMAP = 2, /* index is into filemaps */
};

/*
 * During vfat_init, this only holds the two dummy entries and the
 * directory info. Before the first user query can come in, it will
 * be finalized to include the unused space and the filemaps as well.
 */
std::vector<struct fat_extent> fat_extents;

static uint32_t first_free_cluster(void)
{
	return fat_extents.last().ending_cluster + 1;
}

static uint32_t last_free_cluster(void)
{
	if (filemaps.empty())
		return g_data_clusters + 2 - 1;
	return filemaps.back().starting_cluster - 1;
}

/* Return the index of the extent containing the given cluster number,
 * or -1 if there is no such extent. */
int find_extent(uint32_t clust_nr)
{
	int h, l, m;

	l = 0;
	h = fat_extents.size() - 1;
	while (l <= h) {
		m = (h + l) / 2;
		if (clust_nr < filemaps[m].starting_cluster) {
			h = m - 1;
		} else if (clust_nr > filemaps[m].ending_cluster) {
			l = m + 1;
		} else {
			return m;
		}
	}

	return -1; /* not found */
}

/*
 * Allocate more clusters for the directory starting at clust_nr,
 * if needed.
 */
int extend_dir(uint32_t clust_nr, uint32_t extra_size)
{
	uint32_t clusters_needed;
	uint32_t clusters_now;
	struct dir_info *dir;
	int extent_nr = find_extent(clust_nr);
	struct fat_extent *e;

	if (extent_nr < 0)
		return -1;
	e = &fat_extents[extent_nr];
	if (e->extent_type != EXTENT_DIR)
		return -1;
	dir = &dir_infos[e->index];

	clusters_now = ALIGN(dir->data.size(), CLUSTER_SIZE) / CLUSTER_SIZE;
	clusters_needed = ALIGN(dir->data.size() + extra_size, CLUSTER_SIZE)
		/ CLUSTER_SIZE - clusters_now;
	if (clusters_needed > 0) {
		struct fat_extent *last_e;
		struct fat_extent new_extent;

		last_e = &fat_extents[dir->last_extent];
		if (dir->last_extent == fat_extents.size() - 1) {
			/* yay, shortcut: just extend this extent */
			last_e->ending_cluster += clusters_needed;
			dir->last_cluster = last_e->ending_cluster;
			return 0;
		}

		/* TODO: this is wrong. The last entry of last_e should
		 * point at the new extent. The "terminated" boolean does
		 * not really make sense because what should the last entry
		 * of an unterminated extent contain?
		 * I guess a 'value' field is needed. */

		/* otherwise, allocate a new extent */
		last_e->terminated = 0;
		memset(new_extent, 0, sizeof(new_extent));
		new_extent.starting_cluster = first_free_cluster();
		new_extent.ending_cluster = new_extent.starting_cluster
			+ clusters_needed - 1;
		new_extent.index = e->index;
		new_extent.offset = clusters_now * CLUSTER_SIZE;
		new_extent.extent_type = EXTENT_DIR;
		new_extent.terminated = 1;
		fat_extents.push_back(new_extent);

		dir->last_extent = fat_extents.size() - 1;
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

void encode_date(uint8_t *buf, time_t stamp)  /* 2-byte buffer */
{
	struct tm *t = gmtime(&stamp);
	uint16_t date_part;

	/* struct tm measures years from 1900, but FAT measures from 1980 */
	date_part = (t->tm_mday) | ((t->tm_mon + 1) << 5)
		| ((t->tm_year - 80) << 9);

	buf[0] = date_part & 0xff;
	buf[1] = (date_part >> 8) & 0xff;
}

int add_dir_entry(uint32_t parent_clust, uint32_t entry_clust,
	const filename_t &filename, uint32_t file_size, bool is_dir,
	time_t mtime, time_t atime)
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
	/* special case for the root directory, which is found in cluster 2
	 * but which must be referred to as cluster 0 in directory entries,
	 * so it's convenient to correct for it here so that callers don't
	 * have to. */
	if (parent_clust == 0)
		parent_clust = 2;

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
	encode_date(&short_entry[18], atime); /* 2 bytes */
	short_entry[20] = (entry_clust >> 16) & 0xff;
	short_entry[21] = (entry_clust >> 24) & 0xff;
	encode_datetime(&short_entry[22], mtime);
	short_entry[26] = entry_clust & 0xff;
	short_entry[27] = (entry_clust >> 8) & 0xff;
	short_entry[28] = file_size & 0xff;
	short_entry[29] = (file_size >> 8) & 0xff;
	short_entry[30] = (file_size >> 16) & 0xff;
	short_entry[31] = (file_size >> 24) & 0xff;

	parent = &dir_clusters[parent_clust - 2];
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

	new_cluster.starting_cluster = first_free_cluster();
	new_cluster.last_cluster = new_cluster.starting_cluster;
	new_cluster.cluster_offset = 0;
	new_cluster.path = strdup(path);

	fat_beginning.push_back(htole32(FAT_END_OF_CHAIN));
	dir_clusters.push_back(new_cluster);
	return new_cluster.starting_cluster;
}

uint32_t map_file(const char *name, uint32_t size)
{
	uint32_t nr_clust = ALIGN(size, CLUSTER_SIZE) / CLUSTER_SIZE;
	struct filemap_info fm;

	fm.ending_cluster = last_free_cluster();
	fm.starting_cluster = fm.ending_cluster - nr_clust + 1;
	fm.path = strdup(name);

	filemaps.push_back(fm);
	return fm.starting_cluster;
}

/* Return the number of the file mapped to the given cluster number,
 * or -1 if there is no such file. */
int filemap_find(uint32_t clust_nr)
{
	int h, l, m;

	/* short-circuit common case */
	if (clust_nr <= last_free_cluster())
		return -1;

	l = 0;
	h = filemaps.size() - 1;
	while (l <= h) {
		m = (h + l) / 2;
		/* If this seems reversed, it's because filemaps are
		 * sorted by descending starting cluster */
		if (clust_nr < filemaps[m].starting_cluster) {
			l = m + 1;
		} else if (clust_nr > filemaps[m].ending_cluster) {
			h = m - 1;
		} else {
			return m;
		}
	}

	return -1; /* not found */
}

/*
 * The FAT sectors are deduced from the stored file and directory info,
 * and are created on demand in this function.
 */
void fat_fill(void *vbuf, uint32_t entry_nr, uint32_t entries)
{
	uint32_t *buf = (uint32_t *)vbuf;
	uint32_t i = 0;
	uint32_t first_file = last_free_cluster() + 1;
	uint32_t first_blocked = min(first_file,
		first_free_cluster() + g_max_free_clusters);
	uint32_t count;
	int file_idx;

	if (entry_nr < fat_beginning.size()) {
		count = min(entries, fat_beginning.size() - entry_nr);
		memcpy(vbuf, (void *) &fat_beginning[0], count * 4);
		i = count;
	}

	/*
	 * The unused space between the directories and the files is
	 * divided into an unallocated part and a marked-unusable part.
	 * This is so that we don't report more free space than the
	 * host filesystem actually has.
	 */
	if (entry_nr + i < first_blocked) {
		/* unallocated clusters */
		count = min(entries - i, first_blocked - entry_nr - i);
		memset(&buf[i], 0, count * 4);
		i += count;
	}

	if (entry_nr + i < first_file) {
		/* unusable clusters */
		count = min(entries - i, first_file - entry_nr - i);
		while (count--)
			buf[i++] = htole32(FAT_BAD_CLUSTER);
	}

	if (i == entries)
		return;

	file_idx = filemap_find(entry_nr + i);
	/* TODO: feels like this bit could be simplified */
	if (file_idx >= 0) {
		struct filemap_info *fm = &filemaps[file_idx];
		for ( ; i < entries; i++) {
			if (entry_nr + i == fm->ending_cluster) {
				buf[i] = htole32(FAT_END_OF_CHAIN);
				if (fm == &filemaps[0]) { // past end
					i++;
					break;
				}
				fm--;
			} else {
				buf[i] = htole32(entry_nr + i + 1);
			}
		}
	}

	/* Past end of data clusters. The FAT can still extend here
	 * because there might be unused space in the last FAT sector.
	 * There doesn't seem to be a spec about what should be in that
	 * unused space, but filling it with "bad cluster" markers
	 * seems sensible. */
	for (; i < entries; i++) {
		buf[i] = htole32(FAT_BAD_CLUSTER);
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

int file_fill(void *buf, uint32_t len, uint32_t clust_nr, uint32_t offset,
	uint32_t *filled)
{
	int ret = 0;
	int fd = -1;
	int file_idx = filemap_find(clust_nr);

	if (file_idx < 0) {
		*filled = min(len, CLUSTER_SIZE - offset);
		memset(buf, 0, *filled);
	} else {
		struct filemap_info & fm = filemaps[file_idx];
		int nread;
		int file_offset = (clust_nr - fm.starting_cluster)
			* CLUSTER_SIZE + offset;
		*filled = min(len, (fm.ending_cluster + 1 - clust_nr)
			* CLUSTER_SIZE - offset);

		fd = open(fm.path, O_RDONLY);
		if (fd < 0)
			goto err;

		if (lseek(fd, file_offset, SEEK_SET) == (off_t) -1)
			goto err;

		nread = read(fd, buf, *filled);
		if (nread < 0)
			goto err;

		/* past size of file? */
		if ((uint32_t) nread < *filled)
			memset((char *) buf + nread, 0, *filled - nread);
		close(fd);
	}

	return 0;

err:
	ret = errno;
	if (fd >= 0)
		close(fd);
	return ret;
}

int vfat_fill(void *buf, uint64_t from, uint32_t len)
{
	int ret = 0;

	/*
	 * This is structured as a loop so that each clause can handle
	 * just the case it's focused on and then pass the buck
	 * by setting maxcopy smaller than len.
	 */
	while (len > 0 && ret == 0) {
		uint32_t maxcopy = 0;
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
			uint64_t adj = from - (RESERVED_SECTORS + g_fat_sectors)
				* SECTOR_SIZE;
			uint32_t data_cluster = (adj / CLUSTER_SIZE) + 2;
			uint32_t offset = adj % CLUSTER_SIZE;
			if (data_cluster < first_free_cluster()) {
				maxcopy = min(len, CLUSTER_SIZE - offset);
				dir_fill(buf, data_cluster, offset, maxcopy);
			} else if (data_cluster <= last_free_cluster()) {
				maxcopy = min(len, (uint64_t)
					(last_free_cluster() + 1
					- data_cluster) * CLUSTER_SIZE
					- offset);
				memset(buf, 0, maxcopy);
			} else {
				ret = file_fill(buf, len, data_cluster,	
					offset, &maxcopy);
			}
		} else {
			/* past end of image */
			ret = EINVAL;
		}
		len -= maxcopy;
		buf = (char *) buf + maxcopy;
		from += maxcopy;
	}

	if (ret && len)
		memset(buf, 0, len);
	return ret;
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

static int convert_name(const char *name8, int namelen, filename_t & name16)
{
	ConversionResult result;
	const uint8_t *inp = (const uint8_t *) name8;
	uint16_t *bufp;

	/* VFAT filenames have to be in UTF-16. Linux filenames
	 * aren't in any particular encoding, but nearly all
	 * systems use UTF-8 these days. */

	/* name16 is passed in to receive the result. */
	name16.clear();
	/* The worst case is that name8 is pure ASCII so each byte
	 * expands to one 16-bit element, so make enough space for that
	 * plus a terminating NUL */
	name16.resize(namelen + 1);

	bufp = &name16[0];

	result = ConvertUTF8toUTF16LE(&inp, inp + namelen,
		&bufp, bufp + namelen, strictConversion);
	if (result != conversionOK)
		return -1;

	/* The conversion routine will have set bufp to point just
	 * past the end of the converted data. Anything past that
	 * will be junk, so shrink the name to leave that out. */
	*bufp++ = 0;
	name16.resize(bufp - &name16[0]);
	return 0;
}

static void scan_fts(FTS *ftsp, FTSENT *entp)
{
	uint32_t clust;
	uint32_t parent;
	off_t size;
	filename_t name;

	/*
	 * The scan makes use of entp->fts_number, which is a field
	 * reserved for our use. For directories we store the cluster
	 * number there, so that we can look it up when scanning the
	 * directory's children. The field is initialized to 0, so
	 * 0 means the root directory.
	 */
	switch (entp->fts_info) {
		case FTS_D: /* directory, first visit */
			if (entp->fts_level == 0) /* root dir is already made */
				break;
			if (convert_name(entp->fts_name,
				entp->fts_namelen, name) < 0) {
				/* directory name couldn't be represented.
				 * skip it and its children. */
				fts_set(ftsp, entp, FTS_SKIP);
				break;
			}
			clust = alloc_new_dir(entp->fts_path);
			parent = entp->fts_parent->fts_number;
			
			/* link the new directory into the hierarchy */
			add_dir_entry(clust, clust, dot_name, 0, true,
				entp->fts_statp->st_mtime,
				entp->fts_statp->st_atime);
			add_dir_entry(clust, parent, dot_dot_name, 0, true,
				entp->fts_parent->fts_statp->st_mtime,
				entp->fts_parent->fts_statp->st_atime);
			add_dir_entry(parent, clust, name, 0, true,
				entp->fts_statp->st_mtime,
				entp->fts_statp->st_atime);
			entp->fts_number = clust;
			break;

		case FTS_F: /* normal file */
			size = entp->fts_statp->st_size;
			if ((uint32_t) size != size)
				break;  /* can't represent size */
			if (convert_name(entp->fts_name,
				entp->fts_namelen, name) < 0)
				break;  /* can't represent name */
			parent = entp->fts_parent->fts_number;
			if (size > 0)
				clust = map_file(entp->fts_path, size);
			else
				clust = 0;
			add_dir_entry(parent, clust, name, size, false,
				entp->fts_statp->st_mtime,
				entp->fts_statp->st_atime);
			break;

		case FTS_DP: /* directory, second visit (after all children) */
			break;

		default:
			/* Ignore anything else: unstattable files,
			 * unreadable directories, symbolic links, etc.
			 * It won't be representable in the FAT anyway. */
			break;
	}
}

void scan_target_dir(void)
{
	FTS *ftsp;
	FTSENT *entp;
	/* fts_open takes a (char * const *) array, and there's no
	 * way to get a (const char *) into that array without forcing
	 * something. fts_open does intend to leave the strings alone,
	 * so it's safe. */
	char *path_argv[] = { (char *) g_top_dir, 0 };

	/* FTS is a glibc helper for scanning directory trees */
	ftsp = fts_open(path_argv, FTS_PHYSICAL | FTS_XDEV, NULL);
	while ((entp = fts_read(ftsp)))
		scan_fts(ftsp, entp);

	fts_close(ftsp);
	
}

void vfat_init(const char *target_dir, uint64_t free_space)
{
	const struct fat_extent entry_0 = {
		0, 0, 0x0ffffff8, 0, EXTENT_LITERAL, 0
	};
	const struct fat_extent entry_1 = {
		0, 0, FAT_END_OF_CHAIN, 0, EXTENT_LITERAL, 0
	};

	g_top_dir = target_dir;
	g_max_free_clusters = free_space / CLUSTER_SIZE;

	init_boot_sector();
	init_fsinfo_sector();

	/* entry 0 contains the media descriptor in its low byte,
	 * should be the same as in the boot sector. */
	fat_extents.push_back(entry_0);
	/* entry 1 contains the end-of-chain marker */
	fat_extents.push_back(entry_1);

	alloc_new_dir("."); /* create empty root directory */

	dot_name.clear();
	dot_name.push_back(htole16('.'));
	dot_name.push_back(0);

	dot_dot_name.clear();
	dot_dot_name.push_back(htole16('.'));
	dot_dot_name.push_back(htole16('.'));
	dot_dot_name.push_back(0);

	scan_target_dir();
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
	fprintf(stderr, "Image size %lu sectors, %lu reserved, %lu FAT\n",
		(unsigned long) g_total_sectors,
		(unsigned long) RESERVED_SECTORS,
		(unsigned long) g_fat_sectors);
	fprintf(stderr, "Sector size %d, cluster size %d\n",
		SECTOR_SIZE, CLUSTER_SIZE);
	fprintf(stderr, "Contains %lu data clusters starting at 0x%llx\n",
		(unsigned long) g_data_clusters,
		(unsigned long long) (RESERVED_SECTORS + g_fat_sectors) * SECTOR_SIZE);
	return g_total_sectors;
}
