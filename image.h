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

/*
 * This module keeps track of the image contents.
 * Most of it is not stored literally but as references
 * to data-fill functions from the other modules.
 */

#include <stdint.h>

#include "refcounted.h"

/*
 * Call this before calling any of the other image functions
 */
void image_init();

/*
 * Abstract base class for objects that represent a range of data in
 * the image.
 * It is refcounted, and the image module takes a ref for each data
 * range for which an object is registered. It may also take extra
 * refs if a range is split up by later registrations.
 */
class DataService : public Refcounted {
public:
    /* Subclasses may also want to implement final_deref(), see refcounted.h */

    /*
     * Fill 'buf' with 'length' bytes of data from this object's logical
     * bytestream, starting from 'offset' in that bytestream.
     * Return 0 for success or an errno for failure.
     */
    virtual int fill(char *buf, uint32_t length, uint64_t offset) = 0;

    /*
     * Accept 'length' bytes of new data for this object's logical
     * bytestream starting from 'offset' in that bytestream.
     * Return 0 to accept the data or an errno to reject it.
     * If the data is accepted, the image module will store it.
     */
    virtual int receive(const char *buf, uint32_t length, uint64_t offset) = 0;
};

/*
 * Mark a range of the image data as being provided by 'service'.
 * Future requests for data in this range will be handled by calling
 * service->fill.
 *
 * The 'offset' is the logical offset at the start of this data
 * range, from the service's point of view. It can be used to
 * conveniently register parts of a logical bytestream at different
 * locations, such as with fragmented files.
 *
 * The image module will hold at least one ref to the service as long
 * as the service is responsible for any part of the image.
 */
void image_register(DataService *service, uint64_t start, uint64_t length, uint64_t offset);

/*
 * Accept data written to the image, and store it for future image_fill
 * requests. The data will override any registered fill services
 * for the covered range until it is cleared with image_clear_data().
 *
 * If the data range overlaps with any registered fill services,
 * service->receive will be called for each overlap before the
 * data is stored in the image. Once the data is stored, it will
 * override the service until there is an image_clear or an
 * image_register for that range.
 *
 * If any of the service->receive calls return an error value,
 * the data will not be stored and image_receive will return
 * the error.
 *
 * Return 0 for success or an errno for failure.
 */
int image_receive(const char *buf, uint64_t start, uint32_t length);

/*
 * Fill 'buf' with data from the image, using the registered fill
 * functions and the data from image_receive. Undefined parts of the
 * image will be all zeroes.
 * Return 0 for success or an errno for failure.
 */
int image_fill(char *buf, uint64_t start, uint32_t length);

/*
 * Throw away received data in the specified part of the image.
 * Any data services registered for that range become visible to
 * image_fill again.
 */
void image_clear_data(uint64_t start, uint64_t length);

/*
 * Remove any data services registered in the specified part of the image.
 * Data services that only partly overlap this range will remain valid
 * for their remaining range.
 * Data services whose entire range is removed will be dereferenced.
 */
void image_clear_services(uint64_t start, uint64_t length);
