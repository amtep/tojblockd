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

#include <vector>

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
 * These are valid in the construction phase
 */

/* Reserve a cluster for a new directory and return its number. */
uint32_t fat_alloc_dir(int dir_nr);

/* Reserve 'clusters' clusters for a mapped file and return the first. */
uint32_t fat_alloc_filemap(int filemap_nr, uint32_t clusters);

/* Add 'clusters' clusters to the FAT chain starting at 'cluster_nr'
 * Return true for success */
bool fat_extend(uint32_t cluster_nr, uint32_t clusters);

/* Return the dir number of a directory at this data cluster,
 * or -1 if there is no directory there */
int fat_dir_index(uint32_t cluster_nr);

/* Transition from construction stage to full service. */
void fat_finalize(uint32_t max_free_clusters);

/*
 * These are valid after construction is finalized
 */

/* Write 'entries' FAT entries to 'vbuf', starting from 'entry_nr' */
void fat_fill(void *vbuf, uint32_t entry_nr, uint32_t entries);

/* Fill all or part of 'buf' with data from the image, starting from
 * byte 'offset' at data cluster 'start_clust'. The length may span
 * multiple clusters, but the function does not have to fill more than
 * to the end of the starting cluster.
 * Result: return 0 for success or errno for failure,
 *         and leave the number of bytes in *filled */
int data_fill(char *buf, uint32_t len, uint32_t start_clust, uint32_t offset,
	uint32_t *filled);

/* Interpret 'entries' FAT entries to 'vbuf', starting from 'entry_nr',
 * make adjustments to the file mappings and directories if conclusions
 * can be drawn, and store the entries for future reads. */
int fat_receive(const uint32_t *buf, uint32_t entry_nr, uint32_t entries);
