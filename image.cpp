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

#include "image.h"

#include <assert.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <vector>

using std::min;

struct service_range {
	// The start position is used as the map key so isn't needed here
	uint64_t length;
	uint64_t offset;
	DataService *service;
};

// Indexed by the start position, length is value.length
typedef std::map<uint64_t, struct service_range> servicemap;
servicemap services;

// Indexed by the start position, length is value.size()
typedef std::map<uint64_t, std::vector<char> > datamap;
datamap chunks;

// Implementation note: indexing the services and chunks by their end
// positions would remove the need for find_chunk and find_range, but
// would make everything else more complicated. Tried and wasn't worth it.

// Return an iterator pointing at the chunk containing pos or,
// if there is no such chunk, the first chunk starting after pos.
// May return chunks.end() if there's no qualifying chunk.
static datamap::iterator find_chunk(uint64_t pos)
{
	// Find the first chunk starting at or after pos
	datamap::iterator it = chunks.lower_bound(pos);
	if (it != chunks.begin()) {
		// it points after pos so the chunk containing pos may
		// be the one before it.
		datamap::iterator prev = it;
		--prev;
		if (prev->first + prev->second.size() > pos)
			return prev;
	}
	return it;
}

// Same as find_chunk but for service ranges
static servicemap::iterator find_service(uint64_t pos)
{
	servicemap::iterator it = services.lower_bound(pos);
	if (it != services.begin()) {
		servicemap::iterator prev = it;
		--prev;
		if (prev->first + prev->second.length > pos)
			return prev;
	}
	return it;
}

void image_init()
{
	services.clear();
	chunks.clear();
}

void image_register(DataService *service, uint64_t start,
		uint64_t length, uint64_t offset)
{
	service->ref();
	if (length == 0) {
		// ref then deref is not the same as skipping both,
		// because service might have been handed to us
		// right after construction with 0 refs active.
		service->deref();
		return;
	}
	image_clear_services(start, length);
	struct service_range new_range = {
		length, offset, service
	};
	// After image_clear_services this key will not exist, so it's
	// safe to just assign.
	services[start] = new_range;
}

static int notify_services(const char *buf, uint64_t start, uint32_t length)
{
	uint64_t end = start + length;
	servicemap::iterator s_it = find_service(start);
	for ( ; s_it != services.end() && s_it->first < end; ++s_it) {
		service_range &range = s_it->second;
		uint64_t off = 0; // offset from this range's start
		uint64_t bufpos = 0; // offset in buf
		if (s_it->first < start) {
			// the buffer starts partway through this range
			off = start - s_it->first;
		} else {
			bufpos = s_it->first - start;
		}
		uint64_t len = min(range.length - off, end - s_it->first);
		int ret = range.service->receive(buf + bufpos, len,
					range.offset + off);
		if (ret != 0)
			return ret;
	}

	return 0;
}

int image_receive(const char *buf, uint64_t start, uint32_t length)
{
	if (length == 0)
		return 0;

	int ret = notify_services(buf, start, length);
	if (ret != 0)
		return ret;

	// TODO: try to reuse already-allocated vectors in this range
	image_clear_data(start, length);
	std::vector<char> & chunk = chunks[start];
	chunk.resize(length);
	memcpy(&chunk.front(), buf, length);
	return 0;
}

int image_fill(char *buf, uint64_t start, uint32_t length)
{
	datamap::iterator d_it = find_chunk(start);
	servicemap::iterator s_it = find_service(start);

	for (uint32_t filled = 0; filled < length; ) {
		uint32_t max_len = length - filled;
		char *fill_at = buf + filled;
		uint64_t pos = start + filled;

		// First try filling from a data chunk, which has priority
		if (d_it != chunks.end()) {
			if (d_it->first <= pos) {
				assert(pos - d_it->first <= 0xffffffff);
				uint32_t copy_off = pos - d_it->first;
				uint32_t fill_len = min(d_it->second.size() - copy_off, max_len);
				memcpy(fill_at, &d_it->second.front() + copy_off,
						fill_len);
				filled += fill_len;
				++d_it;
				continue;
			}
			max_len = min(d_it->first - pos, (uint64_t) max_len);
		}

		// Then try filling from a data service
		if (s_it != services.end()) {
			if (s_it->first <= pos) {
				uint64_t fill_off = pos - s_it->first;
				service_range &range = s_it->second;
				if (range.length <= fill_off) {
					// This can happen if filling via d_it
					// ran ahead of s_it
					++s_it;
					continue;
				}
				uint32_t fill_len = min(range.length - fill_off,
						(uint64_t) max_len);
				int ret = range.service->fill(fill_at, fill_len,
						range.offset + fill_off);
				if (ret != 0)
					return ret;
				filled += fill_len;
				++s_it;
				continue;
			}
			max_len = min(s_it->first - pos, (uint64_t) max_len);
		}

		// Nothing defined for this range.
		memset(fill_at, 0, max_len);
		filled += max_len;
	}

	return 0; // success
}

void image_clear_data(uint64_t start, uint64_t length)
{
	if (length == 0)
		return;
	datamap::iterator it = find_chunk(start);
	while (it != chunks.end()) {
		const uint64_t & range_start = it->first;
		std::vector<char> & data = it->second;

		if (range_start >= start + length)
			break;

		if (range_start + data.size() > start + length) {
			// This chunk sticks out past the end of the
			// cleared range, so insert a new chunk
			// starting there.
			uint64_t new_length = range_start + data.size()
					- (start + length);
			std::vector<char> &chunk = chunks[start + length];
			chunk.resize(new_length);
			memcpy(&chunk.front(), &data.back() - new_length,
					new_length);
		}

		if (range_start < start) {
			// This chunk overlaps with the start of the
			// cleared range, so delete its tail.
			data.resize(start - range_start);
			it++;
		} else {
			chunks.erase(it++);
		}
	}
}

void image_clear_services(uint64_t start, uint64_t length)
{
	if (length == 0)
		return;
	servicemap::iterator it = find_service(start);
	while (it != services.end()) {
		const uint64_t range_start = it->first;
		service_range &range = it->second;

		if (range_start >= start + length)
			break;

		if (range_start + range.length > start + length) {
			uint64_t new_start = start + length;
			uint64_t new_length = range_start + range.length
					- new_start;
			struct service_range new_part = {
				new_length,
				range.offset + new_start - range_start,
				range.service
			};
			range.service->ref();
			services[new_start] = new_part;
		}

		if (range_start < start) {
			range.length = start - range_start;
			it++;
		} else {
			range.service->deref();
			services.erase(it++);
		}
	}
}
