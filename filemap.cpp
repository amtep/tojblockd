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
#include "image.h"

class Filemap : public DataService {
public:
	Filemap(const char *path) : path(strdup(path)) { }
	virtual int fill(char *buf, uint32_t length, uint64_t offset);
	virtual int receive(const char *buf, uint32_t length, uint64_t offset);

	const char *path; /* path in real filesystem */
};

uint32_t filemap_add(const char *name, uint32_t size)
{
	uint32_t nr_clust = ALIGN(size, CLUSTER_SIZE) / CLUSTER_SIZE;
	uint32_t starting_cluster = fat_alloc_end(nr_clust);
	Filemap *service = new Filemap(name);
	image_register(service, fat_cluster_pos(starting_cluster), size, 0);
	return starting_cluster;
}

int Filemap::fill(char *buf, uint32_t length, uint64_t offset)
{
	int nread;
	int fd;
	int ret = 0;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return errno;

	if (offset > 0 && lseek(fd, offset, SEEK_SET) == (off_t) -1) {
		ret = errno;
	} else {
		nread = read(fd, buf, length);
		if (nread < 0) {
			ret = errno;
		} else if ((uint32_t) nread < length) { // reached end of file
			// Btw this should only happen if the file was
			// truncated after being registered
			memset(buf + nread, 0, length - nread);
		}
	}
	close(fd);
	return ret;
}

int Filemap::receive(const char *buf, uint32_t length, uint64_t offset)
{
	// TODO: Accept these writes when in a known consistent state?
	return EACCES;
}
