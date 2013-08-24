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

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <unistd.h>

#include <sys/mount.h>  /* BLKROSET */
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>

#include "nbd.h"
#include "udf.h"

#ifndef PROGRAM_NAME
#define PROGRAM_NAME "tojblockd"
#endif

#ifndef PROGRAM_VERSION
#define PROGRAM_VERSION "experimental"
#endif

static int opt_help;
static int opt_version;
static int opt_daemonize;
static const char *opt_device = "/dev/nbd0";
static const char *program_name;

static struct option options[] = {
	{ "help", no_argument, &opt_help, 1 },
	{ "version", no_argument, &opt_version, 1 },
	{ "daemonize", no_argument, &opt_daemonize, 1 },
	{ "device", required_argument, NULL, 'd' },

	{ 0, 0, 0, 0 }
};

void usage(FILE *out)
{
	fprintf(out, "Usage: %s [options] DIRECTORY\n"
		"or: %s --help\n"
		"or: %s --version\n"
		"  Options:\n"
		"  --daemonize  Fork away from the shell and run as a daemon\n"
		"  --device=DEVICE  Open the given network block device\n"
		"      instead of the default /dev/nbd0\n"
		"This program will read a directory (and its subdirectories)\n"
		"and present it as a network block device in UDF format.\n"
		"The network block device can then be mounted normally.\n"
		"The intended use is to export the block device as a raw\n"
		"device (for example via the USB mass storage function)\n"
		"without interfering with normal use of the directory.\n"
		"Limitations:\n"
		"  * Currently read-only\n"
		"  * Files created while the program runs may not be included\n"
		"    in the UDF image\n"
		, program_name, program_name, program_name);
}

void info(const char *fmt, ...)
{
	va_list va;
	fprintf(stderr, "%s: ", program_name);
	va_start(va, fmt);
	vfprintf(stderr, fmt, va);
	va_end(va);
}

void warning(const char *fmt, ...)
{
	va_list va;
	fprintf(stderr, "%s: warning: ", program_name);
	va_start(va, fmt);
	vfprintf(stderr, fmt, va);
	va_end(va);
}

void fatal(const char *fmt, ...)
{
	va_list va;
	fprintf(stderr, "%s: error: ", program_name);
	va_start(va, fmt);
	vfprintf(stderr, fmt, va);
	va_end(va);
	exit(1);
}

void set_read_only(int dev_fd)
{
	int read_only = 1;
	if (ioctl(dev_fd, BLKROSET, &read_only) < 0)
		warning("could not set read-only mode\n");
}

void set_block_size(int dev_fd, unsigned long size)
{
	/*
	 * Failure to set the block size is fatal because there's
	 * no ioctl to query the block size, so if we can't set it
	 * then it's unknown which makes other things impossible later.
	 */
	if (ioctl(dev_fd, NBD_SET_BLKSIZE, size) < 0)
		fatal("could not set block size to %lu\n", size);
}

uint64_t set_image_size(int dev_fd, uint64_t size, int block_size)
{
	uint32_t blocks;
	if (size > (uint64_t) block_size * 0xFFFFFFFF)
		fatal("image size %llu too large", size);
	blocks = size / block_size;
	if (size & (block_size - 1))
		blocks++;

	if (ioctl(dev_fd, NBD_SET_SIZE_BLOCKS, blocks) < 0)
		fatal("could not set image size\n");
	return blocks * block_size;
}

void use_socket(int dev_fd, int sock_fd)
{
	if (ioctl(dev_fd, NBD_SET_SOCK, sock_fd) < 0)
		fatal("could not associate socket with device: %s",
			strerror(errno));
}

void serve(int sock_fd)
{
	sleep(60); /* STUB */
}

void parse_opts(int argc, char **argv)
{
	int c;

	program_name = argv[0];

	while ((c = getopt_long(argc, argv, "", options, NULL) >= 0)) {
		if (c == '?') /* getopt already printed an error msg */
			exit(2);
		if (c == 'd') /* --device */
			opt_device = optarg;
	}
}

int main(int argc, char **argv)
{
	const char *target_dir = 0;
	struct statvfs target_st;
	uint64_t image_size = 0;
	uint64_t free_space = 0;
	int block_size = 4096;
	int dev_fd = -1;
	int sv[2];

	parse_opts(argc, argv);

	if (opt_help) {
		usage(stdout);
		exit(0);
	}

	if (opt_version) {
		printf(PROGRAM_NAME " " PROGRAM_VERSION "\n");
		exit(0);
	}

	if (argc - optind != 1) { /* Expect one DIRECTORY argument */
		usage(stderr);
		exit(2);
	}
	target_dir = argv[optind];

	dev_fd = open(opt_device, O_RDWR);
	if (dev_fd < 0)
		fatal("could not open %s: %s\n", opt_device, strerror(errno));

	if (statvfs(target_dir, &target_st) < 0)
		fatal("could not stat directory tree at %s: %s\n",
			target_dir, strerror(errno));

	image_size = (uint64_t) target_st.f_frsize * target_st.f_blocks;
	free_space = (uint64_t) target_st.f_frsize * target_st.f_bavail;

	set_read_only(dev_fd); /* only read-only is supported, for now */
	set_block_size(dev_fd, block_size);
	image_size = set_image_size(dev_fd, image_size, block_size);

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
		fatal("could not open socket pair: %s", strerror(errno));

	/* TODO: --daemonize */

	if (fork()) {
		/* child */

		close(sv[0]);
		init_udf(target_dir, image_size, free_space);
		serve(sv[1]);
	} else {
		/* parent */
		close(sv[1]);
		use_socket(dev_fd, sv[0]);
		if (ioctl(dev_fd, NBD_DO_IT) < 0)
			fatal("%s processing failed: %s",
				opt_device, strerror(errno));
	}
}
