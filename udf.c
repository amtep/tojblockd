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

#include <errno.h>
#include <string.h>

static const char *g_top_dir;
static uint64_t g_image_size;
static uint64_t g_free_space;

#define UNUSED(a) ((void)(a));

#define ALIGN(x, sz) (((x) + (sz) - 1) & ~((typeof(x))(sz) - 1))

/* TODO: indexed data structure for extents */

typedef int (* extent_fill_t)(void *buf, int64_t from, int32_t len, void *config);;

struct udf_extent {
	struct udf_extent *next;
	uint64_t start;
	uint64_t len;
	void *config;
	extent_fill_t extent_fill;
};

static struct udf_extent *extents;

int zero_fill(void *buf, int64_t from, int32_t len, void *config)
{
	UNUSED(from);
	UNUSED(config);

	memset(buf, 0, len);
	return 0;
}

int buffer_fill(void *buf, int64_t from, int32_t len, void *config)
{
	memcpy(buf, config + from, len);
	return 0;
}

static void set_extent(uint64_t start, uint64_t len, void *config, extent_fill_t extent_fill)
{
	struct udf_extent *new_extent = malloc(sizeof(*new_extent));
	new_extent->start = start;
	new_extent->len = len;
	new_extent->config = config;
	new_extent->extent_fill = extent_fill;
	new_extent->next = extents;
	extents = new_extent;
}

static struct udf_extent *find_extent(uint64_t start)
{
	struct udf_extent *p;

	for (p = extents; p; p = p->next) {
		if (start >= p->start && start < p->start + p->len)
			return p;
	}
	return 0;
}

static void create_volume_recognition_area(void)
{
	/*
	 * The volume recognition area starts 32k into the volume
	 * (the first 32k are reserved for the operating system)
	 * and consists of a series of volume structure descriptors.
	 * Volume structure descriptors always start on a sector boundary.
	 */
	int vsd = ALIGN(32 * 1024, SECTOR_SIZE);
	int vsd_size = ALIGN(2048, SECTOR_SIZE);

	/*
	 * According to UDF/2.60 2.1.7, there should be a single NSR
	 * descriptor in the Extended Area and nothing else.
	 * The Extended Area is marked by the BEA and TEA descriptors.
	 * (A Boot Descriptor is also allowed, but is not needed here.)
	 */

	set_extent(vsd, 7, "\0BEA01\1", buffer_fill); /* ECMA-167 2/9.2 */
	vsd += vsd_size;

	/*
	 * NSR03 indicates the use of ECMA-167/3 (the 1997 version).
	 * NSR02 would indicate the use of ECMA-167/2 (the 1994 version).
	 */
	set_extent(vsd, 7, "\0NSR03\1", buffer_fill); /* ECMA-167 3/9.1 */
	vsd += vsd_size;

	set_extent(vsd, 7, "\0TEA01\1", buffer_fill); /* ECMA-167 2/9.3 */

	/* The sector after the last vsd is reserved and should be zeroed */
}

void init_udf(const char *target_dir, uint64_t image_size, uint64_t free_space)
{
	g_top_dir = target_dir;
	g_image_size = image_size;
	g_free_space = free_space;

	set_extent(0, image_size, 0, zero_fill);

	create_volume_recognition_area();
}

int udf_fill(void *buf, uint64_t from, uint32_t len)
{
	while (len > 0) {
		int ret;
		struct udf_extent *p = find_extent(from);

		if (!p)
			return EINVAL;
		uint32_t max_len = p->start + p->len - from;
		if (max_len > len)
			max_len = len;
		ret = p->extent_fill(buf, from - p->start, max_len, p->config);
		if (ret) {
			memset(buf, 0, len);
			return ret;
		}
		len -= max_len;
		buf += max_len;
		from += max_len;
	}
	return 0;
}
