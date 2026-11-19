/*
 * Crossnode wire protocol, version 2.
 *
 * Every frame is a fixed 28-byte header followed by `len` payload bytes. On a link started with a key (-k), every
 * frame after the handshake is followed by a 32-byte integrity tag (CN_TAG_LEN, see cn_auth.h); with -I there is no
 * handshake and no tag.
 * Both ends are little-endian QNX targets (x86_64 and aarch64le), so fields travel as they are.
 */
#ifndef CN_PROTO_H
#define CN_PROTO_H

#include <stdint.h>

#define CN_MAGIC        0x32304e43u     /* "CN02" as little-endian bytes (version 2: tagged frames) */
#define CN_MAX_DATA     16384u          /* largest read, write or devctl payload */
#define CN_MAX_PATH     1024u           /* longest path, without the terminating NUL */
#define CN_MAX_HANDLES  256             /* open remote files per connection */

enum cn_op {
    CN_OP_PING   = 1,   /* liveness probe; the agent answers at once */
    CN_OP_OPEN   = 2,   /* payload: path relative to "/"; a: open flags */
    CN_OP_READ   = 3,   /* handle; a: byte count */
    CN_OP_WRITE  = 4,   /* handle; payload: the bytes */
    CN_OP_DEVCTL = 5,   /* handle; a: dcmd; b: byte count; payload: input bytes */
    CN_OP_CLOSE  = 6,   /* handle; never answered */
    CN_OP_CANCEL = 7,   /* a: the seq of a request whose caller gave up; never answered */
    CN_OP_STAT   = 8,   /* handle >= 0: fstat of that handle; handle < 0: payload is a path (may be empty = root).
                           Reply payload: struct cn_stat */
    CN_OP_READDIR = 9,  /* handle of a directory; a: index of the first entry wanted; b: most payload bytes.
                           Reply payload: b entries (struct cn_dent + name, padded to 8); b == 0 is the end */
    CN_OP_AUTH   = 10,  /* handshake, see cn_auth.h: the agent opens with a challenge (a reply-flagged frame with a
                           nonce), the net side answers with a nonce and a proof, the agent answers with its proof */
};

#define CN_OFLAG_DIR    0x40000000      /* in the flags of an OPEN request: fail with ENOTDIR unless a directory */
#define CN_F_DIR        0x0001u         /* in the flags of an OPEN reply: the object is a directory */

#define CN_REPLY        0x8000u         /* set in op on every reply */

struct cn_hdr {
    uint32_t magic;
    uint16_t op;
    uint16_t flags;     /* request: CN_F_PRIO | the caller's priority; reply: see CN_F_DIR */
    uint32_t seq;       /* request number; the reply carries the same one */
    int32_t  handle;
    int32_t  a;         /* request: see enum cn_op; reply: errno, 0 on success */
    int32_t  b;         /* request: see enum cn_op; reply: value (handle, byte count, devctl info) */
    uint32_t len;       /* payload bytes that follow */
};

_Static_assert(sizeof(struct cn_hdr) == 28, "the wire header is 28 bytes");

/* A request's flags carry the priority of the client thread that made the call, so the sharing machine can run the
 * work at that priority (see crossnode-agent -P). */
#define CN_F_PRIO      0x8000
#define CN_PRIO_MASK   0x00ff

/* The attributes of one object, in fixed-width fields: never a raw struct stat. */
struct cn_stat {
    uint64_t ino;
    uint64_t size;
    int64_t  atime;     /* seconds */
    int64_t  mtime;
    int64_t  ctime;
    uint32_t mode;      /* S_IFMT bits and permissions, in the usual POSIX values */
    uint32_t uid;
    uint32_t gid;
    uint32_t nlink;
    uint32_t rdev;
    uint32_t blksize;
};

_Static_assert(sizeof(struct cn_stat) == 64, "the wire stat is 64 bytes");

/* One directory entry in a READDIR reply: this header, then namelen name bytes (no NUL), then zero
 * padding up to a multiple of 8. */
struct cn_dent {
    uint64_t ino;
    uint32_t namelen;   /* 1 to CN_MAX_NAME */
    uint32_t reserved;  /* 0 */
};

_Static_assert(sizeof(struct cn_dent) == 16, "the wire dirent header is 16 bytes");

#define CN_MAX_NAME     255u            /* longest entry name */
#define CN_DENT_SIZE(namelen) (sizeof(struct cn_dent) + (((size_t)(namelen) + 7u) & ~(size_t)7u))

#endif
