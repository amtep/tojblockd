# Imported code

This directory contains code that was included from other projects
and was not written for tojblockd.

## ConvertUTF from Unicode, Inc.

Functions for converting between UTF-8 (used in linux filenames)
and UTF-16 (used in VFAT filenames). They have been patched to
always provide little-endian output regardless of host byte order.

## nbd.h from the Linux kernel

Interface definitions for interacting with the nbd module.
This header contains only the bare necessities: struct definitions,
ioctl values, and magic constants.

## sd_notify from systemd

A library function for sending progress indicators to systemd over
a unix dgram socket.
