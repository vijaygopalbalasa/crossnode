/*
 * Crossnode peer authentication: HMAC-SHA256 with a pre-shared key, in plain C (no library).
 *
 * Handshake, once per connection, before any request is served:
 *   agent -> net : a fresh random nonce Na (16 bytes)
 *   net -> agent : a fresh nonce Nc and  MAC('C', Na, Nc)
 *   agent -> net : MAC('A', Nc, Na)         (so the net side knows the agent holds the key too)
 * MAC(role, x, y) = HMAC-SHA256(key, "crossnode-v1" || role || x || y). The key never travels, and both
 * nonces are fresh per connection, so a recorded exchange cannot be replayed.
 *
 * This authenticates the two ends. It does not encrypt or integrity-protect the traffic after the
 * handshake: on an untrusted network, run it over a protected link.
 */
#ifndef CN_AUTH_H
#define CN_AUTH_H

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CN_NONCE_LEN 16
#define CN_MAC_LEN   32

struct cn_sha256 {
    uint32_t h[8];
    unsigned char buf[64];
    uint64_t total;
    size_t fill;
};

static const uint32_t cn_sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define CN_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static inline void cn_sha256_block(struct cn_sha256 *s, const unsigned char *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = CN_ROR(w[i - 15], 7) ^ CN_ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = CN_ROR(w[i - 2], 17) ^ CN_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (CN_ROR(e, 6) ^ CN_ROR(e, 11) ^ CN_ROR(e, 25)) + ((e & f) ^ (~e & g)) + cn_sha_k[i] + w[i];
        uint32_t t2 = (CN_ROR(a, 2) ^ CN_ROR(a, 13) ^ CN_ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static inline void cn_sha256_init(struct cn_sha256 *s)
{
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, iv, sizeof iv);
    s->total = 0;
    s->fill = 0;
}

static inline void cn_sha256_update(struct cn_sha256 *s, const unsigned char *p, size_t n)
{
    s->total += n;
    while (n > 0) {
        size_t take = 64 - s->fill < n ? 64 - s->fill : n;
        memcpy(s->buf + s->fill, p, take);
        s->fill += take;
        p += take;
        n -= take;
        if (s->fill == 64) {
            cn_sha256_block(s, s->buf);
            s->fill = 0;
        }
    }
}

static inline void cn_sha256_final(struct cn_sha256 *s, unsigned char out[32])
{
    uint64_t bits = s->total * 8;
    unsigned char pad = 0x80;
    cn_sha256_update(s, &pad, 1);
    pad = 0;
    while (s->fill != 56)
        cn_sha256_update(s, &pad, 1);
    unsigned char len[8];
    for (int i = 0; i < 8; i++)
        len[i] = (unsigned char)(bits >> (56 - 8 * i));
    cn_sha256_update(s, len, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (unsigned char)(s->h[i] >> 24);
        out[4 * i + 1] = (unsigned char)(s->h[i] >> 16);
        out[4 * i + 2] = (unsigned char)(s->h[i] >> 8);
        out[4 * i + 3] = (unsigned char)s->h[i];
    }
}

static inline void cn_hmac_sha256_parts(const unsigned char *key, size_t klen, const unsigned char *const *part,
                                        const size_t *plen, int nparts, unsigned char out[32])
{
    unsigned char k[64], ipad[64], opad[64], inner[32];
    struct cn_sha256 s;
    memset(k, 0, sizeof k);
    if (klen > 64) {
        cn_sha256_init(&s);
        cn_sha256_update(&s, key, klen);
        cn_sha256_final(&s, k);
    } else {
        memcpy(k, key, klen);
    }
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    cn_sha256_init(&s);
    cn_sha256_update(&s, ipad, 64);
    for (int i = 0; i < nparts; i++)
        cn_sha256_update(&s, part[i], plen[i]);
    cn_sha256_final(&s, inner);
    cn_sha256_init(&s);
    cn_sha256_update(&s, opad, 64);
    cn_sha256_update(&s, inner, 32);
    cn_sha256_final(&s, out);
}

static inline void cn_hmac_sha256(const unsigned char *key, size_t klen, const unsigned char *msg, size_t n,
                                  unsigned char out[32])
{
    const unsigned char *parts[1] = { msg };
    size_t lens[1] = { n };
    cn_hmac_sha256_parts(key, klen, parts, lens, 1, out);
}

/* role 'C' for the net side's proof, 'A' for the agent's; x and y are the two nonces in the order named above. */
static inline void cn_auth_mac(const unsigned char *key, size_t klen, char role, const unsigned char *x,
                               const unsigned char *y, unsigned char out[32])
{
    static const unsigned char tag[] = "crossnode-v1";
    unsigned char r = (unsigned char)role;
    const unsigned char *parts[4] = { tag, &r, x, y };
    size_t lens[4] = { sizeof tag - 1, 1, CN_NONCE_LEN, CN_NONCE_LEN };
    cn_hmac_sha256_parts(key, klen, parts, lens, 4, out);
}

static inline int cn_ct_equal(const unsigned char *a, const unsigned char *b, size_t n)
{
    unsigned char d = 0;
    for (size_t i = 0; i < n; i++)
        d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

#define CN_KEY_MIN 16
#define CN_KEY_MAX 256

/* Fresh random bytes from the system. Returns 0 on success. */
static inline int cn_random(unsigned char *out, size_t n)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return -1;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, out + got, n - got);
        if (r <= 0) {
            close(fd);
            return -1;
        }
        got += (size_t)r;
    }
    close(fd);
    return 0;
}

/* Read a key file: at least CN_KEY_MIN bytes, and not readable or writable by group or others.
 * Returns the length, or -1 with a message in err. */
static inline int cn_load_key(const char *path, unsigned char *key, char *err, size_t errlen)
{
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0) {
        snprintf(err, errlen, "cannot open key file %s: %s", path, strerror(errno));
        return -1;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        snprintf(err, errlen, "key file %s is not a regular file", path);
        return -1;
    }
    if (st.st_mode & 077) {
        close(fd);
        snprintf(err, errlen, "key file %s is readable by others (mode %o); chmod 600 it", path, (unsigned)(st.st_mode & 0777));
        return -1;
    }
    ssize_t n = read(fd, key, CN_KEY_MAX);
    close(fd);
    if (n < CN_KEY_MIN) {
        snprintf(err, errlen, "key file %s is shorter than %d bytes", path, CN_KEY_MIN);
        return -1;
    }
    return (int)n;
}

#endif
