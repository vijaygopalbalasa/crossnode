/* Host-side check of src/cn_auth.h against published HMAC-SHA256 test vectors (RFC 4231, cases 1 and 2,
 * and case 6 with a key longer than the block), plus the handshake helpers.
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

int main(void)
{
    unsigned char mac[32];
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
    printf("%s\n", fails ? "FAILED" : "ALL PASSED");
    return fails != 0;
}
