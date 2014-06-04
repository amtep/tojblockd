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

#include "dir.h"

#include <string.h>
#include <errno.h>

#include <algorithm>
#include <map>

#include "vfat.h"
#include "fat.h"
#include "image.h"

#define DIR_ENTRY_SIZE 32
#define CHARS_PER_DIR_ENTRY 13

/*
 * Directories are allocated from the start of the FAT, but to make
 * the scanning code simpler they don't have to be allocated contiguously
 * the way mapped files are.
 */

class DirService : public DataService {
public:
	DirService(const char *path, uint32_t last_cluster)
		: path(strdup(path)), last_cluster(last_cluster) {}
	virtual int fill(char *buf, uint32_t length, uint64_t offset);
	virtual int receive(const char *buf, uint32_t length, uint64_t offset);

	const char *path; /* path in real filesystem */
	std::vector<char> data;
	uint32_t last_cluster;
};

static uint32_t unique_name_counter = 1;

/* Map starting clusters to directories */
/*
 * TODO: in some sense the image module has this information already.
 * Should it get an image_get_service lookup function?
 * Revisit this after rw support is working.
 */
std::map<uint32_t, DirService *> dirservices;

static void fill_filename_part(char *data, int seq_nr, bool is_last,
	const filename_t &filename, uint8_t checksum)
{
	static const int char_offsets[CHARS_PER_DIR_ENTRY] =
		{ 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
	int i;
	int fn_offset;
	const uint16_t *fn_data;
	int max_i;

	if (is_last)
		data[0] = seq_nr | 0x40;
	else
		data[0] = seq_nr;
	data[11] = FAT_ATTR_LFN;
	data[12] = 0;  /* reserved */
	data[13] = checksum;
	data[26] = 0;  /* cluster nr (unused) */
	data[27] = 0;  /* cluster nr (unused) */

	/* TODO: this loop could be speeded up by special-casing is_last,
	 * because that's the only time that the upper bound might change */

	fn_offset = (seq_nr - 1) * CHARS_PER_DIR_ENTRY;
	fn_data = &filename[0];
	max_i = std::min(CHARS_PER_DIR_ENTRY, (int) filename.size() - fn_offset);
	for (i = 0; i < max_i; i++, fn_offset++) {
		data[char_offsets[i]] = fn_data[fn_offset] & 0xff;
		data[char_offsets[i] + 1] = fn_data[fn_offset] >> 8;
	}
	for ( ; i < CHARS_PER_DIR_ENTRY; i++) {
		data[char_offsets[i]] = 0xff;
		data[char_offsets[i] + 1] = 0xff;
	}
}

static uint8_t calc_vfat_checksum(uint8_t *entry)
{
	uint8_t sum = 0;
	int i;

	for (i = 0; i < 11; i++) {
		sum = ((sum & 1) << 7) + (sum >> 1) + entry[i];
	}
	return sum;
}

/*
 * Fill in just enough of the short entry to be able to calculate the checksum
 */
static void prep_short_entry(uint8_t *entry)  /* at least 11-byte buffer */
{
	uint32_t uniq = unique_name_counter++;
	int i;

	/* The first 11 bytes are the shortname buffer.
	 * Fill it with an invalid but still unique value.
	 * See http://lkml.org/lkml/2009/6/26/313 for the algorithm.
	 */

	entry[0] = ' ';
	entry[1] = 0;
	for (i = 2; i < 8; i++) {
		entry[i] = uniq & 0x1f;
		uniq >>= 5;
	}
	entry[8] = '/';
	entry[9] = 0;
	entry[10] = 0;
}

static void encode_datetime(uint8_t *buf, time_t stamp)  /* 4-byte buffer */
{
	struct tm *t = localtime(&stamp);
	uint16_t time_part;
	uint16_t date_part;

	time_part = (t->tm_sec / 2) | (t->tm_min << 5) | (t->tm_hour << 11);
	/* struct tm measures years from 1900, but FAT measures from 1980 */
	date_part = (t->tm_mday) | ((t->tm_mon + 1) << 5)
		| ((t->tm_year - 80) << 9);

	buf[0] = time_part & 0xff;
	buf[1] = (time_part >> 8) & 0xff;
	buf[2] = date_part & 0xff;
	buf[3] = (date_part >> 8) & 0xff;
}

static void encode_date(uint8_t *buf, time_t stamp)  /* 2-byte buffer */
{
	struct tm *t = gmtime(&stamp);
	uint16_t date_part;

	/* struct tm measures years from 1900, but FAT measures from 1980 */
	date_part = (t->tm_mday) | ((t->tm_mon + 1) << 5)
		| ((t->tm_year - 80) << 9);

	buf[0] = date_part & 0xff;
	buf[1] = (date_part >> 8) & 0xff;
}

void dir_init()
{
	unique_name_counter = 1;
	dirservices.clear();
	dir_alloc_new("."); /* create empty root directory */
}

bool dir_add_entry(uint32_t parent_clust, uint32_t entry_clust,
	const filename_t &filename, uint32_t file_size, uint8_t attrs,
	time_t mtime, time_t atime)
{
	DirService *parent;
	int num_entries;
	uint32_t allocated;
	int seq_nr;
	int data_offset;
	uint8_t checksum;
	uint8_t short_entry[DIR_ENTRY_SIZE];

	/* special case for the root directory, which is found in cluster 2
	 * but which must be referred to as cluster 0 in directory entries,
	 * so it's convenient to correct for it here so that callers don't
	 * have to. */
	if (parent_clust == 0)
		parent_clust = ROOT_DIR_CLUSTER;

        /* filesystem spec limitation: 255 characters plus terminator */
        if (filename.size() > 256)
		return false;

	if (dirservices.count(parent_clust) == 0)
		return false; /* no such directory */
	parent = dirservices[parent_clust];

	/* Check if the result will fit in the allocated space */
	allocated = ALIGN(parent->data.size(), CLUSTER_SIZE);
	if (allocated == 0)
		allocated = CLUSTER_SIZE; // even empty dirs get one cluster
	/* add one entry for the shortname */
	num_entries = 1 + (filename.size() + CHARS_PER_DIR_ENTRY - 1)
				/ CHARS_PER_DIR_ENTRY;
	if (num_entries * DIR_ENTRY_SIZE + parent->data.size() > allocated) {
		uint32_t new_last = fat_extend_chain(parent->last_cluster);
		if (new_last == 0)
			return false;
		image_register(parent, fat_cluster_pos(new_last), CLUSTER_SIZE,
				allocated);
		parent->last_cluster = new_last;
	}

	prep_short_entry(short_entry);
	attrs |= FAT_ATTR_READ_ONLY;  /* always read-only */
	if (attrs & FAT_ATTR_DIRECTORY)
		file_size = 0;
	short_entry[11] = attrs;
	short_entry[12] = 0;
	/* Slightly higher resolution creation time.
	 * The normal time format only encodes down to 2-second precision. */
	short_entry[13] = (mtime & 1) * 100;
	/* this field calls for creation time but we don't have that, so
	 * substitute last modification time */
	encode_datetime(&short_entry[14], mtime);  /* 4 bytes */
	encode_date(&short_entry[18], atime); /* 2 bytes */
	short_entry[20] = (entry_clust >> 16) & 0xff;
	short_entry[21] = (entry_clust >> 24) & 0xff;
	encode_datetime(&short_entry[22], mtime);
	short_entry[26] = entry_clust & 0xff;
	short_entry[27] = (entry_clust >> 8) & 0xff;
	short_entry[28] = file_size & 0xff;
	short_entry[29] = (file_size >> 8) & 0xff;
	short_entry[30] = (file_size >> 16) & 0xff;
	short_entry[31] = (file_size >> 24) & 0xff;

	data_offset = parent->data.size();
	parent->data.resize(parent->data.size() + num_entries * DIR_ENTRY_SIZE);

	checksum = calc_vfat_checksum(short_entry);
	/* The name parts are stored last-to-first, with decreasing seq_nr */
	for (seq_nr = num_entries - 1; seq_nr >= 1; seq_nr--) {
		fill_filename_part(&parent->data[data_offset], seq_nr,
			seq_nr == num_entries - 1, filename, checksum);
		data_offset += 32;
	}

	memcpy(&parent->data[data_offset], short_entry, DIR_ENTRY_SIZE);
	return true;
}

uint32_t dir_alloc_new(const char *path)
{
	uint32_t starting_cluster = fat_alloc_beginning(1);
	DirService *service = new DirService(path, starting_cluster);
	service->ref();
	dirservices[starting_cluster] = service;
	image_register(service, fat_cluster_pos(starting_cluster),
			CLUSTER_SIZE, 0);

	return starting_cluster;
}

int DirService::fill(char *buf, uint32_t length, uint64_t offset)
{
	uint32_t extra = 0;
	if (offset + length > data.size()) {
		extra = offset + length - data.size();
		if (extra > length)
			extra = length;
	}
	memcpy(buf, &data.front() + offset, length - extra);
	memset(buf + length - extra, 0, extra);
	return 0;
}

int DirService::receive(const char *buf, uint32_t length, uint64_t offset)
{
	// TODO: STUB
	return 0;
}
