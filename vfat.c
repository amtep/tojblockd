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

#include <errno.h>
#include <string.h>

#include "sectorspace.h"

#define __unused __attribute__((unused))
#include <bsd/sys/tree.h>

#define CLUSTER_SIZE 4096  /* must be a power of two */
#define SECTORS_PER_CLUSTER (CLUSTER_SIZE / SECTOR_SIZE)
#define RESERVED_SECTORS 32  /* before first FAT */

static const char *g_top_dir;
static uint64_t g_image_size;
static uint64_t g_free_space;

struct sector_info {
	RB_ENTRY(sector_info) rb;
	uint32_t sector_nr;
	void *data;
};

static RB_HEAD(sector_tree, sector_info) sector_data;

static inline int sector_cmp(struct sector_info *a, struct sector_info *b)
{
	/* can't just subtract because the result might not fit in an int */
	return a->sector_nr > b->sector_nr ? 1
		: a->sector_nr < b->sector_nr ? -1
		: 0;
}

RB_PROTOTYPE_STATIC(sector_tree, sector_info, rb, sector_cmp);
RB_GENERATE_STATIC(sector_tree, sector_info, rb, sector_cmp);

/* space_used keeps track of space that has been reserved or filled */
struct sectorspace *space_used;

static void *get_sector(uint32_t sector_nr)
{
	struct sector_info dummy;
	dummy.sector_nr = sector_nr;
	struct sector_info *p = RB_FIND(sector_tree, &sector_data, &dummy);
	if (p)
		return p->data;
	return 0;
}

static void *alloc_sector(uint32_t sector_nr)
{
	struct sector_info *p = malloc(sizeof(*p));
	p->data = malloc(SECTOR_SIZE);
	memset(p->data, 0, SECTOR_SIZE);
	p->sector_nr = sector_nr;
	RB_INSERT(sector_tree, &sector_data, p);
	return p->data;
}

static void record_data(uint64_t start, void *data, uint32_t len)
{
	while (len > 0) {
		uint32_t sector_nr = start / SECTOR_SIZE;
		void *sector = get_sector(sector_nr);
		if (!sector)
			sector = alloc_sector(sector_nr);
		uint32_t offset = start % SECTOR_SIZE;
		uint32_t max_len = SECTOR_SIZE - offset;
		if (max_len > len)
			max_len = len;
		memcpy(sector + offset, data, max_len);
		start += max_len;
		data += max_len;
		len -= max_len;
	}
}

int vfat_fill(void *buf, uint64_t from, uint32_t len)
{
	int ret = -EINVAL;
	while (len > 0) {
		uint32_t sector_nr = from / SECTOR_SIZE;
		uint32_t offset = from % SECTOR_SIZE;
		uint32_t max_len;
		void *sector;

		if (from >= g_image_size)
			goto err;

		max_len = SECTOR_SIZE - offset;
		if (max_len > len)
			max_len = len;

		sector = get_sector(sector_nr);
		if (sector)
			memcpy(buf, sector + offset, max_len);
		else
			memset(buf, 0, max_len);
		len -= max_len;
		buf += max_len;
		from += max_len;
	}
	return 0;

err:
	memset(buf, 0, len);
	return ret;
}

/* Erase sectors containing this range. Warning, it wipes whole sectors! */
static void erase_data(uint64_t start, uint32_t len)
{
	struct sector_info dummy;
	dummy.sector_nr = start / SECTOR_SIZE;

	while (len > 0) {
		struct sector_info *p = RB_FIND(sector_tree, &sector_data, &dummy);
		if (p) {
			p = RB_REMOVE(sector_tree, &sector_data, p);
			free(p->data);
			free(p);
		}
		if (len >= SECTOR_SIZE)
			len -= SECTOR_SIZE;
		else
			len = 0;
		dummy.sector_nr++;
	}
}

void vfat_init(const char *target_dir, uint64_t image_size, uint64_t free_space)
{
	g_top_dir = target_dir;
	g_image_size = image_size;
	g_free_space = free_space;

	space_used = init_sectorspace(0, image_size);
}

void vfat_size_ok(uint32_t sectors, uint32_t sector_size)
{
	uint32_t data_clusters;
	uint32_t fat_sectors;
	if (sector_size != SECTOR_SIZE)
		return 0;

	/* Where did this formula come from? MAGIC! */
	/* well actually it was by subsituting the one for fat_sectors into
         * data_clusters == (sectors - reserved_sectors - fat_sectors)
	 *                  / SECTORS_PER_CLUSTER
	 * and simplifying. */
	/* This formula can still be off by one because simplifying some
	 * of the integer divisions left a remainder of up to CLUSTER_SIZE-1
	 * before the division by (CLUSTER_SIZE + 4). */
	data_clusters = ((sectors + 1 - RESERVED_SECTORS) * SECTOR_SIZE + 7
		+ CLUSTER_SIZE - 1) / (CLUSTER_SIZE + 4)
	fat_sectors = ((data_clusters + 2) * 4 + SECTOR_SIZE - 1) / SECTOR_SIZE;
	if (RESERVED_SECTORS + fat_sectors + data_clusters * SECTORS_PER_CLUSTER
		> sectors) {
		/* leaving the remainder in the formula was too optimistic */
		data_clusters--;
		fat_sectors = ((data_clusters + 2) * 4 + SECTOR_SIZE - 1) / SECTOR_SIZE;
	}
	/* work in progress */

	return 1;
}
