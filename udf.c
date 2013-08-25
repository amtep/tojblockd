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

struct udf_extent_ad { /* ECMA-167 3/7.1 */
	uint32_t length;
	uint32_t location;
};

struct udf_descriptor_tag { /* ECMA-167 3/7.2 */
	uint16_t identifier;
	uint16_t version;
	uint8_t checksum;
	uint8_t reserved;
	uint16_t serial;
	uint16_t crc;
	uint16_t crc_length;
	uint32_t location;
};

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

enum descriptor_tag_type {
	/* Listed in ECMA-167 3/7.2.1 */
	PRIMARY_VD_TAG = 1,
	ANCHOR_VD_POINTER_TAG = 2,
	VD_POINTER_TAG = 3,
	IMPLEMENTATION_USE_VD_TAG = 4,
	PARTITION_DESCRIPTOR_TAG = 5,
	LOGICAL_VD_TAG = 6,
	UNALLOCATED_SPACE_DESCRIPTOR_TAG = 7,
	TERMINATING_DESCRIPTOR_TAG = 8,
	LOGICAL_VOLUME_INTEGRITY_DESCRIPTOR_TAG = 9
};

/*
 * Terminology note: the "descriptor" is the whole thing being written,
 * usually 512 bytes or more. The "descriptor tag" is a 16-byte header at
 * the start of it. The tag contains some checksums which is why it's
 * only filled in here, just before recording.  See ECMA-167 3/7.2
 *
 * Not all descriptors follow this format. Only use record_descriptor()
 * for the ones that start with the standard tag format.
 */
static void record_descriptor(uint64_t pos, void *buf, uint16_t id, uint32_t len)
{
	struct udf_descriptor_tag *tag = buf;
	uint8_t *p = buf;
	int i;

	tag->identifier = htole16(id);
	tag->version = 3; /* version of ECMA-167 in use */
	tag->checksum = 0;
	tag->reserved = 0;
	tag->serial = 0; /* incremented when reusing old media; N/A here */
	tag->crc_length = len - sizeof(*tag);
	tag->location = htole32(pos / SECTOR_SIZE);

	tag->crc = htole16(udf_crc(buf + sizeof(*tag), tag->crc_length));

	/* checksum is a simple modulo-256 sum of the tag bytes */
	for (i = 0; i < 16; i++)
		tag->checksum += *p++;

	record_data(pos, buf, len);
}

static void fill_extent_ad(struct udf_extent_ad *st,
	uint32_t length, uint32_t loc)
{
	/* See ECMA-167 3/7.1 */
	st->length = htole32(length);
	st->location = htole32(loc);
}

/* The caller must have reserved and zeroed the sector for this already */
static void record_anchor_vd_pointer(uint64_t pos,
	uint64_t vds_start, uint32_t vds_len)
{
	/*
	 * This structure gives the location of the volume descriptor
	 * sequence (VDS). See ECMA-167 3/10.2
	 */

	struct anchor_vd_pointer {
		struct udf_descriptor_tag tag;
		struct udf_extent_ad main_vds_extent;
		struct udf_extent_ad reserve_vds_extent;
		char padding[480];
	} st;

	memset(&st, 0, sizeof(st));
	fill_extent_ad(&st.main_vds_extent, vds_len, vds_start / SECTOR_SIZE);
	fill_extent_ad(&st.reserve_vds_extent, 0, 0); /* no reserve copy */
	record_descriptor(pos, &st, ANCHOR_VD_POINTER_TAG, sizeof(st));
}

static void record_volume_data_structures(void)
{
	/*
	 * The volume descriptors are 1 sector each and we need space
	 * for the Primary Volume Descriptor, the Unallocated Space Descriptor,
	 * and the Terminating Descriptor.
	 */
	const int vds_sectors = 3;
	uint32_t last_sector = sectorspace_endsector(space_used);
	uint64_t start;

	/*
	 * The volume descriptors are pointed at by two Anchor Volume
	 * Descriptor Pointer sectors at fixed locations.
	 * Reserve those locations now.
	 */
	sectorspace_mark(space_used, 256 * SECTOR_SIZE, SECTOR_SIZE);
	sectorspace_mark(space_used, (last_sector - 256) * SECTOR_SIZE,
		SECTOR_SIZE);

	/* Pick a handy location for the volume descriptor sequence */
	start = sectorspace_find(space_used, vds_sectors * SECTOR_SIZE);

	//record_primary_volume_descriptor(start);
	//record_unallocated_space_descriptor(start + SECTOR_SIZE);
	//record_terminating_descriptor(start + 2 * SECTOR_SIZE);
	record_anchor_vd_pointer(256 * SECTOR_SIZE,
		start, vds_sectors * SECTOR_SIZE);
	record_anchor_vd_pointer((last_sector - 256) * SECTOR_SIZE,
		start, vds_sectors * SECTOR_SIZE);
}

void init_udf(const char *target_dir, uint64_t image_size, uint64_t free_space)
{
	g_top_dir = target_dir;
	g_image_size = image_size;
	g_free_space = free_space;

	space_used = init_sectorspace(0, image_size);

	record_volume_recognition_area();
	record_volume_data_structures();
}

