/* Host-side check of src/cn_auth.h against published test vectors: SHA-256 (FIPS 180-4 examples: empty, "abc",
 * the two-block message, and one million 'a' fed in uneven pieces), HMAC-SHA256 (RFC 4231, cases 1 and 2, and
 * case 6 with a key longer than the block), plus the handshake and per-frame tag helpers.
 * Build and run: cc -Wall -Wextra -Werror -o /tmp/auth-vectors test/auth-vectors.c && /tmp/auth-vectors */
#include <stdio.h>
#include <string.h>
#include "../src/cn_auth.h"

static int fails;
static void hexcheck(const char *name, const unsigned char *got, const char *want)
{
    char hex[65];
    for (int i = 0; i < 32; i++)
        sprintf(hex + 2 * i, "%02x", got[i]);
    if (strcmp(hex, want) != 0) {
        printf("FAIL %s\n  got  %s\n  want %s\n", name, hex, want);
        fails++;
    } else {
        printf("PASS %s\n", name);
    }
}

static void sha(const char *name, const char *msg, const char *want)
{
    struct cn_sha256 st;
    unsigned char out[32];
    cn_sha256_init(&st);
    cn_sha256_update(&st, (const unsigned char *)msg, strlen(msg));
    cn_sha256_final(&st, out);
    hexcheck(name, out, want);
}

int main(void)
{
    unsigned char mac[32];
    sha("sha256 empty", "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    sha("sha256 abc", "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    sha("sha256 two blocks (448 bits)", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    {
        static unsigned char as[1000];
        memset(as, 'a', sizeof as);
        struct cn_sha256 st;
        unsigned char out[32];
        cn_sha256_init(&st);
        size_t left = 1000000, piece = 1;
        while (left > 0) {          /* pieces of 1, 2, ... 999 bytes, so every block boundary case is crossed */
            size_t n = piece < left ? piece : left;
            cn_sha256_update(&st, as, n);
            left -= n;
            piece = piece % 999 + 1;
        }
        cn_sha256_final(&st, out);
        hexcheck("sha256 one million 'a' in uneven pieces", out,
                 "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }
    unsigned char k1[20];
    memset(k1, 0x0b, sizeof k1);
    cn_hmac_sha256(k1, sizeof k1, (const unsigned char *)"Hi There", 8, mac);
    hexcheck("rfc4231 case 1", mac, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    cn_hmac_sha256((const unsigned char *)"Jefe", 4, (const unsigned char *)"what do ya want for nothing?", 28, mac);
    hexcheck("rfc4231 case 2", mac, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    unsigned char k6[131];
    memset(k6, 0xaa, sizeof k6);
    cn_hmac_sha256(k6, sizeof k6,
                   (const unsigned char *)"Test Using Larger Than Block-Size Key - Hash Key First", 54, mac);
    hexcheck("rfc4231 case 6 (key longer than a block)", mac,
             "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    unsigned char a[32], b[32];
    memset(a, 1, 32);
    memcpy(b, a, 32);
    if (!cn_ct_equal(a, b, 32)) { printf("FAIL ct_equal same\n"); fails++; } else printf("PASS ct_equal same\n");
    b[31] ^= 1;
    if (cn_ct_equal(a, b, 32)) { printf("FAIL ct_equal differs\n"); fails++; } else printf("PASS ct_equal differs\n");
    unsigned char na[16], nc[16], m1[32], m2[32];
    memset(na, 7, 16);
    memset(nc, 9, 16);
    cn_auth_mac(k1, sizeof k1, 'C', na, nc, m1);
    cn_auth_mac(k1, sizeof k1, 'A', nc, na, m2);
    if (cn_ct_equal(m1, m2, 32)) { printf("FAIL client and agent proofs must differ\n"); fails++; } else printf("PASS client and agent proofs differ\n");
    {   /* The per-frame tag binds the frame counter and every payload bit. */
        unsigned char k[32], hdr[28], pl[8], t0[32], t1[32], t2[32];
        memset(k, 3, 32);
        memset(hdr, 5, sizeof hdr);
        memset(pl, 42, sizeof pl);
        cn_frame_tag(k, 7, hdr, pl, sizeof pl, t0);
        cn_frame_tag(k, 8, hdr, pl, sizeof pl, t1);
        pl[0] ^= 1;
        cn_frame_tag(k, 7, hdr, pl, sizeof pl, t2);
        if (cn_ct_equal(t0, t1, 32)) { printf("FAIL frame tag must change with the counter\n"); fails++; } else printf("PASS frame tag changes with the counter (a replay is caught)\n");
        if (cn_ct_equal(t0, t2, 32)) { printf("FAIL frame tag must change with one payload bit\n"); fails++; } else printf("PASS frame tag changes with one payload bit\n");
    }
    printf("%s\n", fails ? "FAILED" : "ALL PASSED");
    return fails != 0;
}
