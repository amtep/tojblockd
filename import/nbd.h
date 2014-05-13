/* Definitions for the binary interface to the network block device */

#include <linux/ioctl.h>

#define NBD_SET_SOCK    _IO( 0xab, 0 )
#define NBD_SET_BLKSIZE _IO( 0xab, 1 )
#define NBD_DO_IT       _IO( 0xab, 3 )
#define NBD_SET_SIZE_BLOCKS     _IO( 0xab, 7 )

#define NBD_REQUEST_MAGIC 0x25609513
#define NBD_REPLY_MAGIC 0x67446698

enum nbd_type {
        NBD_CMD_READ = 0,
        NBD_CMD_WRITE = 1,
        NBD_CMD_DISC = 2,
        NBD_CMD_FLUSH = 3,
        NBD_CMD_TRIM = 4
};

struct nbd_request {
        uint32_t magic; /* NBD_REQUEST_MAGIC */
        uint32_t type; /* nbd_type */
        char handle[8]; /* opaque */
        uint64_t from; /* offset into device, in bytes */
        uint32_t len; /* request length in bytes */
	/* 'len' bytes follow if type is WRITE */
} __attribute__ ((packed));

struct nbd_reply {
        uint32_t magic; /* NBD_REPLY_MAGIC */
        uint32_t error; /* 0 means ok */
        char handle[8]; /* copy from nbd_request */
	/* 'len' bytes follow if replying to a READ request */
};
