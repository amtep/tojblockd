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

/*
 * Relevant standards:
 *
 * Universal Disk Format specification
 *   version 2.60 at http://www.osta.org/specs/pdf/udf260.pdf
 * which is a profile of ECMA-167/3
 *   http://www.ecma-international.org/publications/standards/Ecma-167.htm
 */

#include "udf.h"

#include <stdlib.h>

#include <errno.h>
#include <string.h>

#include "sectorspace.h"

#define __unused __attribute__((unused))
#include <bsd/sys/tree.h>

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

int udf_fill(void *buf, uint64_t from, uint32_t len)
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

/* Create a volume structure descriptor according to ECMA-167 3/9.1 */
/* The id parameter must be 5 chars long */
static void record_volume_structure_descriptor(uint64_t start, const char *id)
{
	unsigned char vsd_header[7];
	/* The total vsd is 2048 bytes long but aside from the header
	 * it's going to be all zeroes. */
	erase_data(start, 2048);

	vsd_header[0] = 0; /* structure type; it's 0 for all known VSDs */
	memcpy(&vsd_header[1], id, 5);
	vsd_header[6] = 1; /* structure version; it's 1 for all known VSDs */

	record_data(start, vsd_header, 7);
}

static void record_volume_recognition_area(void)
{
	/*
	 * The volume recognition area starts 32k into the volume
	 * (the first 32k are reserved for the operating system)
	 * and consists of a series of volume structure descriptors.
	 * Volume structure descriptors always start on a sector boundary.
	 */
	int vsd = ALIGN(32 * 1024, SECTOR_SIZE);
	int vsd_stride = ALIGN(2048, SECTOR_SIZE);

	/*
	 * According to UDF/2.60 2.1.7, there should be a single NSR
	 * descriptor in the Extended Area and nothing else.
	 * The Extended Area is marked by the BEA and TEA descriptors.
	 * (A Boot Descriptor is also allowed, but is not needed here.)
	 * 
	 * NSR03 indicates the use of ECMA-167/3 (the 1997 version).
	 * NSR02 would indicate the use of ECMA-167/2 (the 1994 version).
	 */

	record_volume_structure_descriptor(vsd, "BEA01"); /* ECMA-167 2/9.2 */
	vsd += vsd_stride;
	record_volume_structure_descriptor(vsd, "NSR03"); /* ECMA-167 3/9.1 */
	vsd += vsd_stride;
	record_volume_structure_descriptor(vsd, "TEA01"); /* ECMA-167 2/9.3 */
	vsd += vsd_stride;

	/* The sector after the last vsd is reserved and should remain zeroed */
	erase_data(vsd, SECTOR_SIZE);
	vsd += SECTOR_SIZE;

	/* Reserve the leading 32k and the recognition area */
	sectorspace_mark(space_used, 0, vsd);
}

void init_udf(const char *target_dir, uint64_t image_size, uint64_t free_space)
{
	g_top_dir = target_dir;
	g_image_size = image_size;
	g_free_space = free_space;

	space_used = init_sectorspace(0, image_size);

	record_volume_recognition_area();
}

