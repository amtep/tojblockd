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

#include <stdint.h>

/*
 * This file is the interface to the File Allocation Table logic.
 * Only the FAT32 format is supported here.
 *
 * The FAT uses 4 bytes per data cluster to record allocation.
 * The allocations are singly linked lists, with each entry pointing
 * to the next or being an end marker. The first two entries are dummy
 * and don't refer to data clusters.
 *
 * The FAT created here is always laid out with the directories in
 * the beginning and the files at the end, and all the free space
 * in between.
 *
 * Despite its name, FAT32 only uses 28 bits in its entries.
 * The top 4 bits should be cleared when allocating.
 */

#define RESERVED_FAT_ENTRIES 2

/* These special values are predefined by the FAT specification */
#define FAT_END_OF_CHAIN 0x0fffffff
#define FAT_BAD_CLUSTER  0x0ffffff7
#define FAT_UNALLOCATED  0

/*
 * The FAT interface has two stages. In the first stage, the target
 * directory is scanned and files and directories are allocated
 * in the FAT. When that is done, the FAT is finalized and then
 * it becomes ready to answer requests.
 */

void fat_init(uint32_t data_clusters);

/*
 * Returns the starting position in the image of data cluster 'cluster'
 */
uint64_t fat_cluster_pos(uint32_t cluster);

/*
 * These are valid in the construction phase
 */

/* Allocate a chain of 'clusters' clusters near the beginning of the FAT
 * and return the number of the first allocated cluster. */
uint32_t fat_alloc_beginning(uint32_t clusters);

/* Allocate a chain of 'clusters' clusters near the end of the FAT and
 * return the number of the first allocated cluster. */
uint32_t fat_alloc_end(uint32_t clusters);

/* Add a cluster to the FAT chain containing 'cluster_nr'.
 * Return its new ending cluster for success, or 0 for failure. */
uint32_t fat_extend_chain(uint32_t cluster_nr);

/* Transition from construction stage to full service. */
void fat_finalize(uint32_t max_free_clusters);

/* Return true iff every chain in the FAT has a unique starting point
 * and terminates in an END_OF_CHAIN */
bool fat_is_consistent(void);
