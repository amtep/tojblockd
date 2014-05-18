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

/* This file is included from fat.cpp if TESTING is defined */

#include "fat_check.h"

#include <stdio.h>

const char *fat_check_invariants(void)
{
	static char errmsg[256];

	if (extents.size() < RESERVED_FAT_ENTRIES)
		return "Reserved FAT entries missing";

	uint32_t last_actual = extents.back().ending_cluster;
	uint32_t last_expected = g_data_clusters + RESERVED_FAT_ENTRIES - 1;
	if (last_actual != last_expected) {
		sprintf(errmsg,
			"Last extent does not end at end of data (%u vs %u)",
			last_actual, last_expected);
		return errmsg;
	}

	if (extents[0].starting_cluster != 0) {
		sprintf(errmsg,
			"Extents do not start at 0 (first cluster %u)",
			extents[0].starting_cluster);
		return errmsg;
	}

	uint32_t prev;
	for (int i = 0; i < (int) extents.size(); i++) {
		struct fat_extent *fe = &extents[i];
		if (fe->starting_cluster > fe->ending_cluster) {
			sprintf(errmsg, "Inverted extent %d (%u..%u)",
				i, fe->starting_cluster, fe->ending_cluster);
			return errmsg;
		}
		if (i > 0 && fe->starting_cluster != prev + 1) {
			sprintf(errmsg,
				"Gap between extents (clusters %u and %u)",
				extents[i - 1].ending_cluster,
				fe->starting_cluster);
			return errmsg;
		}
		if (fe->extent_type != EXTENT_LITERAL
			&& (fe->next == FAT_UNALLOCATED
			    || fe->next == FAT_BAD_CLUSTER)) {
			sprintf(errmsg,
				"Extent %d has bad next (0x%x)", i, fe->next);
			return errmsg;
		}
		prev = fe->ending_cluster;
	}
	return 0;
}
