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

#include <string.h>
#include <errno.h>

#include <algorithm>

#include "vfat.h"

/*
 * A fat_extent is a contiguous section of the FAT where the values
 * are either all identical (empty, bad sector, etc) or are ascending
 * numbers where each value points to its neighbor, with possibly
 * an end-of-chain marker at the end.
 */
struct fat_extent {
	uint32_t starting_cluster;
	uint32_t ending_cluster;
	uint32_t index;  /* index to file or dir table, or literal value */
	uint32_t offset; /* byte offset of starting_cluster in file or dir */
	uint32_t next;   /* cluster of next extent, or end-of-chain */
	uint8_t extent_type; /* EXTENT_ values */
};

enum {
	EXTENT_LITERAL = 0, /* "index" is literal value ("next" not used) */
	EXTENT_DIR = 1, /* index is into dir_infos */
	EXTENT_FILEMAP = 2, /* index is into filemaps */
};

/* entry 0 contains the media descriptor in its low byte,
 * should be the same as in the boot sector. */
const struct fat_extent entry_0 = {
	0, 0, 0x0ffffff8, 0, 0, EXTENT_LITERAL
};
/* entry 1 contains the end-of-chain marker */
const struct fat_extent entry_1 = {
	1, 1, FAT_END_OF_CHAIN, 0, 0, EXTENT_LITERAL
};

/*
 * During the construction stage, this contains the two dummy
 * entries and the directories. During finalization, the free
 * space and filemaps are added at the end.
 */
static std::vector<struct fat_extent> extents;
/*
 * During the construction stage, this contains the filemap
 * extents ordered from high to low cluster numbers. This allows
 * efficient appending. After finalization this vector is empty.
 */
static std::vector<struct fat_extent> extents_from_end;

static uint32_t g_data_clusters;

void fat_init(uint32_t data_clusters)
{
	g_data_clusters = data_clusters;
	extents.push_back(entry_0);
	extents.push_back(entry_1);
}

/* This function is only valid during construction stage */
static uint32_t first_free_cluster()
{
	return extents.back().ending_cluster + 1;
}

/* This function is only valid during construction stage */
static uint32_t last_free_cluster()
{
	if (extents_from_end.empty())
		return g_data_clusters + RESERVED_FAT_ENTRIES - 1;
	return extents_from_end.back().starting_cluster - 1;
}

/* Return the index of the extent containing the given cluster number,
 * or -1 if there is no such extent. */
static int find_extent(uint32_t cluster_nr)
{
	int h, l, m;

	l = 0;
	h = extents.size() - 1;
	while (l <= h) {
		m = (h + l) / 2;
		if (cluster_nr < extents[m].starting_cluster) {
			h = m - 1;
		} else if (cluster_nr > extents[m].ending_cluster) {
			l = m + 1;
		} else {
			return m;
		}
	}

	return -1; /* not found */
}

int fat_dir_index(uint32_t cluster_nr)
{
	struct fat_extent *fe;
	int extent_nr = find_extent(cluster_nr);

	if (extent_nr < 0)
		return -1;

	fe = &extents[extent_nr];
	if (fe->extent_type != EXTENT_DIR)
		return -1;

	return fe->index;
}

uint32_t fat_alloc_dir(int dir_nr)
{
	struct fat_extent new_extent;

	new_extent.starting_cluster = first_free_cluster();
	new_extent.ending_cluster = new_extent.starting_cluster;
	new_extent.index = dir_nr;
	new_extent.offset = 0;
	new_extent.next = FAT_END_OF_CHAIN;
	new_extent.extent_type = EXTENT_DIR;

	extents.push_back(new_extent);

	return new_extent.starting_cluster;
}

uint32_t fat_alloc_filemap(int filemap_nr, uint32_t clusters)
{
	struct fat_extent new_extent;

	new_extent.ending_cluster = last_free_cluster();
	new_extent.starting_cluster = new_extent.ending_cluster - clusters + 1;
	new_extent.index = filemap_nr;
	new_extent.offset = 0;
	new_extent.next = FAT_END_OF_CHAIN;
	new_extent.extent_type = EXTENT_FILEMAP;

	extents_from_end.push_back(new_extent);
	return new_extent.starting_cluster;
}

bool fat_extend(uint32_t cluster_nr, uint32_t clusters)
{
	struct fat_extent new_extent;
	struct fat_extent *fe;
	int extent_nr = find_extent(cluster_nr);

	/* Search for last extent of this file or dir */
	while (extent_nr >= 0 && extents[extent_nr].next != FAT_END_OF_CHAIN) {
		/* EXTENT_LITERAL extents are not chained */
		if (extents[extent_nr].extent_type == EXTENT_LITERAL)
			return false;
		extent_nr = find_extent(extents[extent_nr].next);
	}

	if (extent_nr < 0)
		return false;

	if (extent_nr == (int) extents.size() - 1) {
		/* yay, shortcut: just extend this extent */
		extents[extent_nr].ending_cluster += clusters;
		return true;
	}

	fe = &extents[extent_nr];

	new_extent.starting_cluster = first_free_cluster();
	new_extent.ending_cluster = new_extent.starting_cluster + clusters - 1;
	new_extent.index = fe->index;
	new_extent.offset = fe->offset +
		(fe->ending_cluster - fe->starting_cluster + 1) * CLUSTER_SIZE;
	new_extent.next = FAT_END_OF_CHAIN;
	new_extent.extent_type = fe->extent_type;
	fe->next = new_extent.starting_cluster;

	extents.push_back(new_extent);
	return true;
}

void fat_finalize(uint32_t max_free_clusters)
{
	struct fat_extent fe_free;
	struct fat_extent fe_bad;

	/*
	 * The unused space between the directories and the files is
	 * divided into an unallocated part and a marked-unusable part.
	 * This is so that we don't report more free space than the
	 * host filesystem actually has.
	 */

	fe_free.starting_cluster = first_free_cluster();
	fe_free.ending_cluster = std::min(last_free_cluster(),
		first_free_cluster() + max_free_clusters - 1);
	fe_free.index = FAT_UNALLOCATED;
	fe_free.offset = 0;
	fe_free.next = fe_free.index;
	fe_free.extent_type = EXTENT_LITERAL;

	if (fe_free.ending_cluster >= fe_free.starting_cluster)
		extents.push_back(fe_free);

	fe_bad.starting_cluster = fe_free.ending_cluster + 1;
	fe_bad.ending_cluster = last_free_cluster();
	fe_bad.index = FAT_BAD_CLUSTER;
	fe_bad.offset = 0;
	fe_bad.next = fe_bad.index;
	fe_bad.extent_type = EXTENT_LITERAL;

	if (fe_bad.ending_cluster >= fe_bad.starting_cluster)
		extents.push_back(fe_bad);

	int pos = extents.size();
	extents.resize(pos + extents_from_end.size());
	for (int i = (int) extents_from_end.size() - 1; i >= 0; i--, pos++)
		extents[pos] = extents_from_end[i];

	extents_from_end.clear();
}

void fat_fill(void *vbuf, uint32_t entry_nr, uint32_t entries)
{
	uint32_t *buf = (uint32_t *)vbuf;
	uint32_t i = 0;

	int extent_nr = find_extent(entry_nr);
	while (extent_nr >= 0) {
		struct fat_extent *fe = &extents[extent_nr];
		if (fe->extent_type == EXTENT_LITERAL) {
			while (entry_nr + i <= fe->ending_cluster
			       && i < entries)
				buf[i++] = htole32(fe->index);
		} else {
			while (entry_nr + i < fe->ending_cluster
			       && i < entries) {
				buf[i] = htole32(entry_nr + i + 1);
				i++;
			}
			if (i < entries)
				buf[i++] = htole32(fe->next);
		}
		if (i == entries)
			return;
		// This is a fast version of calling find_extent(entry_nr + i)
		// It relies on the extents being contiguous.
		if (extent_nr < (int) extents.size() - 1)
			extent_nr++;
		else
			extent_nr = -1;
	}

	/*
	 * Past end of data clusters. The FAT can still extend here
	 * because there might be unused space in the last FAT sector.
	 * There doesn't seem to be a spec about what should be in that
	 * unused space, but filling it with "bad cluster" markers
	 * seems sensible.
	 */
	for (; i < entries; i++) {
		buf[i] = htole32(FAT_BAD_CLUSTER);
	}
}

int data_fill(char *buf, uint32_t len, uint32_t start_clust, uint32_t offset,
	uint32_t *filled)
{
	int extent_nr = find_extent(start_clust);
	struct fat_extent *fe;
	uint32_t src_offset;
	int ret = 0;

	if (extent_nr < 0)
		return EINVAL;

	fe = &extents[extent_nr];

	// Clip len if the current extent does not go that far
	uint32_t end_clust = start_clust + (offset + len - 1) / CLUSTER_SIZE;
	if (end_clust > fe->ending_cluster)
		len = (fe->ending_cluster - start_clust + 1) * CLUSTER_SIZE
			- offset;

	src_offset = (start_clust - fe->starting_cluster) * CLUSTER_SIZE
		+ fe->offset + offset;

	switch (fe->extent_type) {
		case EXTENT_LITERAL:
			// bad block or unallocated; return 0s either way
			memset(buf, 0, len);
			break;
		case EXTENT_DIR:
			ret = dir_fill(buf, len, fe->index, src_offset);
			break;
		case EXTENT_FILEMAP:
			ret = filemap_fill(buf, len, fe->index, src_offset);
			break;
	}

	*filled = len;
	return ret;
}
