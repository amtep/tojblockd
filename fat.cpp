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

#include <assert.h>
#include <string.h>

#include <errno.h>

#include <algorithm>

#include "vfat.h"
#include "image.h"
#include "dir.h"
#include "filemap.h"

/*
 * A fat_extent is a contiguous section of the FAT where the values
 * are either all identical (empty, bad sector, etc) or are ascending
 * numbers where each value except the last points to its neighbour.
 */
struct fat_extent {
	uint32_t starting_cluster;
	uint32_t ending_cluster;
	uint32_t index;  /* index to file or dir table, or literal value */
	uint32_t next;   /* cluster of next extent, or end-of-chain */
	uint8_t extent_type; /* EXTENT_ values */
};

enum {
	EXTENT_LITERAL = 0, /* "index" is literal value ("next" not used) */
	EXTENT_UNKNOWN = 1, /* unclassified chain written to our image */
};

/* entry 0 contains the media descriptor in its low byte,
 * should be the same as in the boot sector. */
static const struct fat_extent entry_0 = {
	0, 0, 0x0ffffff8, 0, EXTENT_LITERAL
};
/* entry 1 contains the end-of-chain marker */
static const struct fat_extent entry_1 = {
	1, 1, FAT_END_OF_CHAIN, 0, EXTENT_LITERAL
};

/* Just calling vec.clear() does not release the allocated memory. */
template <typename T>
static void clear_vector(std::vector<T> & vec)
{
	std::vector<T>().swap(vec);
}

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
static uint64_t g_fat_size;

void fat_init(uint32_t data_clusters)
{
	g_data_clusters = data_clusters;
	g_fat_size = ALIGN((g_data_clusters + RESERVED_FAT_ENTRIES) * 4,
			SECTOR_SIZE);
	clear_vector(extents);
	extents.push_back(entry_0);
	extents.push_back(entry_1);
	clear_vector(extents_from_end);
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

class FatDataService : public DataService {
	virtual int fill(char *buf, uint32_t length, uint64_t offset);
	virtual int receive(const char *buf, uint32_t length, uint64_t offset);
} fatservice;

/*
 * Split or reuse an extent so that a new single-cluster extent is
 * created for cluster_nr. Return the number of that extent.
 */
// TODO: record that the chain is damaged?
static int punch_extent(uint32_t cluster_nr, uint32_t value)
{
	struct fat_extent new_ext;
	int extent_nr = find_extent(cluster_nr);
	if (extent_nr < 0)
		return -1;

	new_ext.starting_cluster = cluster_nr;
	new_ext.ending_cluster = cluster_nr;
	if (value == FAT_UNALLOCATED || value == FAT_BAD_CLUSTER) {
		new_ext.extent_type = EXTENT_LITERAL;
		new_ext.index = value;
		new_ext.next = 0;
	} else {
		new_ext.extent_type = EXTENT_UNKNOWN;
		new_ext.index = 0;
		new_ext.next = value;
	}

	struct fat_extent *fe = &extents[extent_nr];
	if (fe->starting_cluster == fe->ending_cluster) {
		*fe = new_ext; // re-use
		return extent_nr;
	}

	if (fe->starting_cluster == cluster_nr) {
		fe->starting_cluster++;
		extents.insert(extents.begin() + extent_nr, new_ext);
		return extent_nr;
	} else if (fe->ending_cluster == cluster_nr) {
		fe->ending_cluster--;
		if (fe->extent_type != EXTENT_LITERAL)
			fe->next = cluster_nr;
		extents.insert(extents.begin() + extent_nr + 1, new_ext);
		return extent_nr + 1;
	} else {
		// the extent has to be split in two pieces
		struct fat_extent post_ext;
		post_ext.starting_cluster = cluster_nr + 1;
		post_ext.ending_cluster = fe->ending_cluster;
		post_ext.index = fe->index;
		post_ext.next = fe->next;
		post_ext.extent_type = fe->extent_type;
		fe->ending_cluster = cluster_nr - 1;
		if (fe->extent_type != EXTENT_LITERAL)
			fe->next = cluster_nr;
		// Because of the vector implementation it's more
		// efficient to insert two elements and then fix up
		// one of them, than to do two inserts.
		extents.insert(extents.begin() + extent_nr + 1, 2, post_ext);
		extents[extent_nr + 1] = new_ext;
		return extent_nr + 1;
	}
}

/* Try to add an entry to an extent.
 * Don't worry about the following extent, the caller will patch it up.
 * Return true iff the entry was added. */
static bool try_inc_extent(int extent_nr, uint32_t value)
{
	struct fat_extent *fe = &extents[extent_nr];

	/* Literal extents can be extended with an entry of the same value */
	if (fe->extent_type == EXTENT_LITERAL) {
		if (fe->index == value) {
			fe->ending_cluster++;
			return true;
		}
		return false;
	}

	/* All other extents are chains, which can be extended if the
	 * next pointer was pointing at the following entry anyway.
	 * (This won't happen in a properly constructed FAT, but can
	 * often happen while processing newly allocated chains.) */
	if (fe->next == fe->ending_cluster + 1) {
		fe->next = value;
		fe->ending_cluster++;
		return true;
	}

	return false;
}

/* This extent had its first entry stolen. Adjust it. */
// TODO: record that the chain is damaged?
static void bump_extent(int extent_nr)
{
	struct fat_extent *fe = &extents[extent_nr];

	if (fe->starting_cluster == fe->ending_cluster) {
		extents.erase(extents.begin() + extent_nr);
	} else {
		fe->starting_cluster++;
	}
}

/* Try to change the last entry of this extent.
 * Only do it if it makes sense.
 * Return true iff the change was made. */
static bool try_renext_extent(int extent_nr, uint32_t value)
{
	struct fat_extent *fe = &extents[extent_nr];

	if (extent_nr < RESERVED_FAT_ENTRIES)
		return false;
	if (fe->extent_type == EXTENT_LITERAL)
		return false;

	if (value == FAT_END_OF_CHAIN
		|| (value > RESERVED_FAT_ENTRIES
			&& value < g_data_clusters + RESERVED_FAT_ENTRIES)) {
		// TODO: record that the chain is now open?
		// and that the entry pointed to by the old 'next' is
		// now an unlinked chain?
		fe->next = value;
		return true;
	}
	return false;
}

uint64_t fat_cluster_pos(uint32_t cluster_nr)
{
	return (uint64_t) (RESERVED_SECTORS * SECTOR_SIZE) + g_fat_size
		+ (cluster_nr - 2) * CLUSTER_SIZE;
}

uint32_t fat_alloc_beginning(uint32_t clusters)
{
	struct fat_extent new_extent;

	new_extent.starting_cluster = first_free_cluster();
	new_extent.ending_cluster = new_extent.starting_cluster + clusters - 1;
	new_extent.index = 0;
	new_extent.next = FAT_END_OF_CHAIN;
	new_extent.extent_type = EXTENT_UNKNOWN;

	extents.push_back(new_extent);

	return new_extent.starting_cluster;
}

uint32_t fat_alloc_end(uint32_t clusters)
{
	struct fat_extent new_extent;

	new_extent.ending_cluster = last_free_cluster();
	new_extent.starting_cluster = new_extent.ending_cluster - clusters + 1;
	new_extent.index = 0;
	new_extent.next = FAT_END_OF_CHAIN;
	new_extent.extent_type = EXTENT_UNKNOWN;

	extents_from_end.push_back(new_extent);
	return new_extent.starting_cluster;
}

uint32_t fat_extend_chain(uint32_t cluster_nr)
{
	struct fat_extent new_extent;
	struct fat_extent *fe;
	int extent_nr = find_extent(cluster_nr);

	/* Search for last extent of this file or dir */
	while (extent_nr >= 0 && extents[extent_nr].next != FAT_END_OF_CHAIN) {
		/* EXTENT_LITERAL extents are not chained */
		if (extents[extent_nr].extent_type == EXTENT_LITERAL)
			return 0;
		extent_nr = find_extent(extents[extent_nr].next);
	}

	if (extent_nr < 0)
		return 0;

	if (extent_nr == (int) extents.size() - 1) {
		/* yay, shortcut: just extend this extent */
		return ++extents[extent_nr].ending_cluster;
	}

	fe = &extents[extent_nr];

	new_extent.starting_cluster = first_free_cluster();
	new_extent.ending_cluster = new_extent.starting_cluster;
	new_extent.index = fe->index;
	new_extent.next = FAT_END_OF_CHAIN;
	new_extent.extent_type = fe->extent_type;
	fe->next = new_extent.starting_cluster;

	extents.push_back(new_extent);
	return new_extent.ending_cluster;
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
	fe_free.next = fe_free.index;
	fe_free.extent_type = EXTENT_LITERAL;

	if (fe_free.ending_cluster >= fe_free.starting_cluster)
		extents.push_back(fe_free);

	fe_bad.starting_cluster = fe_free.ending_cluster + 1;
	fe_bad.ending_cluster = last_free_cluster();
	fe_bad.index = FAT_BAD_CLUSTER;
	fe_bad.next = fe_bad.index;
	fe_bad.extent_type = EXTENT_LITERAL;

	if (fe_bad.ending_cluster >= fe_bad.starting_cluster)
		extents.push_back(fe_bad);

	int pos = extents.size();
	extents.resize(pos + extents_from_end.size());
	for (int i = (int) extents_from_end.size() - 1; i >= 0; i--, pos++)
		extents[pos] = extents_from_end[i];

	clear_vector(extents_from_end);

	image_register(&fatservice, RESERVED_SECTORS * SECTOR_SIZE,
			g_fat_size, 0);
}

int FatDataService::fill(char *cbuf, uint32_t length, uint64_t offset)
{
	uint32_t *buf = (uint32_t *) cbuf;
	uint32_t entry_nr = offset / 4;
	uint32_t entries = length / 4;
	uint32_t i = 0;

	assert(offset % 4 == 0);
	assert(length % 4 == 0);

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
			return 0;
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

	return 0;
}

int FatDataService::receive(const char *cbuf, uint32_t length, uint64_t offset)
{
	uint32_t *buf = (uint32_t *) cbuf;
	uint32_t entries = length / 4;
	uint32_t entry_nr = offset / 4;

	assert(length % 4 == 0);
	assert(offset % 4 == 0);

	/* Construct the current FAT, to have something to diff against */
	uint32_t *orig = (uint32_t *)malloc(length);
	this->fill((char *) orig, length, offset);

	for (uint32_t i = 0; i < entries; i++) {
		if (buf[i] == orig[i])
			continue;
		if (entry_nr + i < RESERVED_FAT_ENTRIES)
			return EIO;
		if (orig[i] == htole32(FAT_BAD_CLUSTER))
			return EIO;
		int extent_nr = find_extent(entry_nr + i);
		if (extent_nr <= 0)
			return EIO;
		struct fat_extent *fe = &extents[extent_nr];
		uint32_t value = le32toh(buf[i]);
		if (fe->starting_cluster == entry_nr + i) {
			/* Try adding value to end of previous extent */
			if (try_inc_extent(extent_nr - 1, value)) {
				bump_extent(extent_nr);
				continue;
			}
		}
		if (fe->ending_cluster == entry_nr + i) {
			/* See if value is a reasonable new
			 * next value for this extent */
			if (try_renext_extent(extent_nr, value))
				continue;
		}
		/* split off a new extent for this entry, and record
		 * it as a single-cluster chain */
		extent_nr = punch_extent(entry_nr + i, value);
	}

	free(orig);
	return 0;
}

#ifdef TESTING
#include "tests/fat/fat_check.cpp"
#endif
