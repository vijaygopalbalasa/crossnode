/*
 * Crossnode wire protocol, version 1.
 *
 * Every frame is a fixed 28-byte header followed by `len` payload bytes.
 * Both ends are little-endian QNX targets (x86_64 and aarch64le), so fields travel as they are.
 */
#ifndef CN_PROTO_H
#define CN_PROTO_H

#include <stdint.h>

#define CN_MAGIC        0x31304e43u     /* "CN01" as little-endian bytes */
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
};

#define CN_REPLY        0x8000u         /* set in op on every reply */

struct cn_hdr {
    uint32_t magic;
    uint16_t op;
    uint16_t flags;     /* reserved, 0 */
    uint32_t seq;       /* request number; the reply carries the same one */
    int32_t  handle;
    int32_t  a;         /* request: see enum cn_op; reply: errno, 0 on success */
    int32_t  b;         /* request: see enum cn_op; reply: value (handle, byte count, devctl info) */
    uint32_t len;       /* payload bytes that follow */
};

_Static_assert(sizeof(struct cn_hdr) == 28, "the wire header is 28 bytes");

#endif
