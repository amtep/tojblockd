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
 * A sectorspace keeps track of 'marked' (used or allocated) sectors.
 * Its interface expresses offset and length in bytes, but it only
 * marks space in whole sectors.
 */

#include "sectorspace.h"

#include <stdlib.h>

#include "udf.h"

/*
 * struct sectorspace and struct sector_extent happen to have the
 * same layout, but they describe different things. The start and
 * len in sectorspace describe the whole area available.
 *
 * The start and len in the sector_extent linked list describe
 * individual chunks of that area. None of the extents in the
 * linked list may overlap, but all of them must be in the
 * area described by the sectorspace.
 */

struct sector_extent {
	struct sector_extent *next;
	/* both fields are measured in sectors */
	uint32_t starts;
	uint32_t ends;
};

struct sectorspace {
	struct sector_extent *extents;
	/* both fields are measured in sectors */
	uint32_t starts;
	uint32_t ends;
};

struct sectorspace *init_sectorspace(uint64_t start, uint64_t len)
{
	struct sectorspace *space = malloc(sizeof(*space));
	space->extents = 0;
	space->starts = start / SECTOR_SIZE;
	space->ends = space->starts + len / SECTOR_SIZE - 1;
	return space;
}

/* extent p has just grown; try to absorb extents that now overlap with it */
static void try_merge_following(struct sector_extent *p)
{
	if (!p)
		return;

	while (p->next && p->next->starts <= p->ends + 1) {
		struct sector_extent *t = p->next;
		if (t->ends > p->ends)
			p->ends = t->ends;
		p->next = p->next->next;
		free(t);
	}
}

void sectorspace_mark(struct sectorspace *space, uint64_t start, uint64_t len)
{
	uint32_t starts = start / SECTOR_SIZE;
	uint32_t ends = (start + len - 1) / SECTOR_SIZE;
	struct sector_extent **np = &space->extents;
	struct sector_extent *p;

	if (len == 0)
		return;

	/* extents are kept sorted by their starts field */
	while (*np) {
		p = *np;
		if (p->ends + 1 < starts) {
			*np = p->next;
			continue;
		}
		if (p->starts > ends + 1)
			break;

		/* overlapping or adjacent */

		if (p->starts <= starts) {
			if (ends > p->ends)
				p->ends = ends;
			try_merge_following(p);
			return;
		} else {
			p->starts = starts;
			return;
		}
	}

	/*
	 * Either np is at the end of the list or it points to an extent
	 * that is past the one being marked, so insert a new one here.
	 */
	p = malloc(sizeof(*p));
	p->next = *np;
	p->starts = starts;
	p->ends = ends;
	*np = p;
}

/*
 * Find a contiguous unmarked space of the desired length, mark it and
 * return its offset.
 *
 * Returns 0 if no suitable space is found, even though 0 can also be
 * a valid offset. In practice, 0 will not be a valid offset because
 * it's always reserved in UDF.
 */
uint64_t sectorspace_find(struct sectorspace *space, uint64_t len)
{
	uint32_t scount = (len + SECTOR_SIZE - 1) / SECTOR_SIZE;
	struct sector_extent *p = space->extents;

	while (p && p->next) {
		if (p->next->starts - p->ends >= scount) {
			uint64_t start = (p->ends + 1) * SECTOR_SIZE;
			p->ends += scount;
			try_merge_following(p);
			return start;
		}
	}

	if (p) { /* at end of extents list */
		if (space->ends - p->ends >= scount) {
			uint64_t start = (p->ends + 1) * SECTOR_SIZE;
			p->ends += scount;
			return start;
		}
		return 0; /* no space */
	}

	/* there was no extents list; make the first entry */
	if (space->ends - space->starts + 1 >= scount) {
		p = malloc(sizeof(*p));
		p->next = 0;
		p->starts = space->starts;
		p->ends = space->starts + scount - 1;
		space->extents = p;
		return space->starts * SECTOR_SIZE;
	}

	return 0; /* no space */
}

uint32_t sectorspace_startsector(struct sectorspace *space)
{
	return space->starts;
}

uint32_t sectorspace_endsector(struct sectorspace *space)
{
	return space->ends;
}
