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

void image_init()
{
}

void image_register(DataService *service, uint64_t start,
		uint64_t length, uint64_t offset)
{
}

int image_receive(const char *buf, uint64_t start, uint32_t length)
{
}

int image_fill(char *buf, uint64_t start, uint32_t length)
{
}

void image_clear_data(uint64_t start, uint64_t length)
{
}

void image_clear_services(uint64_t start, uint64_t length)
{
}
