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

#include "filemap.h"

#include <string.h>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <vector>

#include "vfat.h"
#include "fat.h"

struct filemap_info {
	uint32_t starting_cluster;
	const char *path; /* path in real filesystem */
};

/* filemaps are kept sorted by descending starting_cluster */
static std::vector<struct filemap_info> filemaps;

uint32_t filemap_add(const char *name, uint32_t size)
{
	uint32_t nr_clust = ALIGN(size, CLUSTER_SIZE) / CLUSTER_SIZE;
	struct filemap_info fm;

	fm.starting_cluster = fat_alloc_filemap(filemaps.size(), nr_clust);
	fm.path = strdup(name);

	filemaps.push_back(fm);
	return fm.starting_cluster;
}

int filemap_fill(char *buf, uint32_t len, int fmap_index, uint32_t offset)
{
	if (fmap_index < 0 || fmap_index >= (int) filemaps.size())
		return EINVAL;

	const char *path = filemaps[fmap_index].path;
	int nread;
	int fd;
	int ret = 0;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return errno;

	if (lseek(fd, offset, SEEK_SET) == (off_t) -1) {
		ret = errno;
	} else {
		nread = read(fd, buf, len);
		if (nread < 0) {
			ret = errno;
		} else if ((uint32_t) nread < len) { // reached end of file
			memset(buf + nread, 0, len - nread);
		}
	}
	close(fd);
	return ret;
}
