# tojblockd

tojblockd was designed to allow USB devices (such as smartphones) to
act as mass storage devices and to let users manipulate their data files
via mass storage access.

## The problem

Normally, exporting a filesystem as USB mass storage has significant
drawbacks: the filesystem in question has to be unmounted while it's
exported, and it has to be a filesystem type that's suitable for most
desktop computers (in practice, that's VFAT, with UDF as a contender).

This makes exporting the user's home directory problematic, and it means
that files on the exported filesystem become invisible to apps while
the mass storage mode is active.

## The solution

tojblockd takes a directory tree and whips up a VFAT image that can
be exported. It reads the directory tree via the normal filesystem
interface, so there are no limitations on the target directory.

To save resources, the VFAT image is constructed only as needed.
Files from the target directory are just mapped in and are not
read until the USB host asks for them.

tojblockd effectively takes a snapshot of the directory tree and
filenames when it starts up. If files disappear while the directory
is exported, the USB host will get I/O errors when trying to read
their data blocks. If new files are created while the directory
is exported, they won't be visible in the image until tojblockd
is restarted.

The intended configuration is that tojblockd is started whenever
the USB cable is plugged in and the system is switched to mass storage
mode, and tojblockd is stopped again whenever that stops being the
case.

## Configuration

The simplest way to run tojblockd is to start it with a directory
name as argument. It will open `/dev/nbd0` and configure it to
serve as a block device representing that directory tree.

For real use, it must be tied to the USB detection and mode switching
systems. The details will depend on the platform it's running on.
There's no published example setup yet.

## Platform requirements

tojblockd uses Linux-specific interfaces (particularly the network
block device interface) and was developed for use with Nemo Mobile
and Mer.

The kernel it runs on must have the `nbd` driver either compiled in
or inserted as a module.

## License

tojblockd is under the GPLv2+.
See the files LICENSE.md and COPYING for details.
