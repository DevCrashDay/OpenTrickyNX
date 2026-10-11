/*
 * ot_sign_test -- host test of the release signature (ot_sign.c).
 *
 *   cc -std=c99 -O2 -Iport/launcher/app -Iport/third_party/monocypher \
 *      port/launcher/tools/ot_sign_test.c port/launcher/app/ot_sign.c \
 *      port/third_party/monocypher/monocypher.c \
 *      port/third_party/monocypher/optional/monocypher-ed25519.c -o ot_sign_test
 *   ./ot_sign_test                          the cases below; 0 when all pass
 *   ./ot_sign_test <file> <file.sig> <public key, 64 hex>
 *                                           checks one file (tools/publish's
 *                                           test signs with Python, checks here)
 *
 * Keys here are the public test vectors of RFC 8032 (section 7.1), never a
 * key of the project.
 */
#include "ot_sign.h"
#include "optional/monocypher-ed25519.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

static void hex(const char *h, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(h + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static void expect(const char *what, OtSigResult got, OtSigResult want)
{
    printf("%-44s %s (%s)\n", what, got == want ? "ok" : "FAIL", ot_sign_text(got));
    if (got != want) fails++;
}

static int read_all(const char *path, uint8_t **data, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t cap = 1 << 16, len = 0;
    uint8_t *d = (uint8_t *)malloc(cap);
    size_t got;
    while (d && (got = fread(d + len, 1, cap - len, f)) > 0) {
        len += got;
        if (len == cap) d = (uint8_t *)realloc(d, cap *= 2);
    }
    fclose(f);
    *data = d;
    *n = len;
    return d != NULL;
}

static int check_files(const char *file, const char *sigfile, const char *keyhex)
{
    uint8_t key[32], *msg = NULL, *sig = NULL;
    size_t msg_n = 0, sig_n = 0;
    if (strlen(keyhex) != 64) { fprintf(stderr, "the key is 64 hex digits\n"); return 2; }
    hex(keyhex, key, 32);
    if (!read_all(file, &msg, &msg_n)) { fprintf(stderr, "cannot read %s\n", file); return 2; }
    int have_sig = read_all(sigfile, &sig, &sig_n);
    OtSigResult r = ot_sign_check(msg, msg_n, have_sig ? sig : NULL, sig_n, key);
    printf("%s: %s\n", file, ot_sign_text(r));
    free(msg);
    free(sig);
    return r == OT_SIG_OK ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 4) return check_files(argv[1], argv[2], argv[3]);

    uint8_t pub1[32], sig1[64], pub2[32], sig2[64], bad[64];
    /* RFC 8032 7.1, TEST 1 (empty message) and TEST 2 (one byte, 0x72). */
    hex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", pub1, 32);
    hex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
        "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b", sig1, 64);
    hex("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", pub2, 32);
    hex("92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
        "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00", sig2, 64);
    const uint8_t m2[1] = { 0x72 };
    expect("RFC 8032 test 1: valid", ot_sign_check("", 0, sig1, 64, pub1), OT_SIG_OK);
    expect("RFC 8032 test 2: valid", ot_sign_check(m2, 1, sig2, 64, pub2), OT_SIG_OK);
    expect("test 2 signature, test 1 key: invalid", ot_sign_check(m2, 1, sig2, 64, pub1), OT_SIG_INVALID);

    /* A SHA256SUMS.txt signed with the RFC's test 1 secret seed. */
    uint8_t seed[32], sk[64], pk[32], sig[64];
    hex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", seed, 32);
    crypto_ed25519_key_pair(sk, pk, seed);
    if (memcmp(pk, pub1, 32) != 0) { printf("key pair from the RFC seed: FAIL\n"); fails++; }
    char sums[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef  OpenTricky-0.1.3-win64.zip\n";
    size_t n = strlen(sums);
    crypto_ed25519_sign(sig, sk, (const uint8_t *)sums, n);
    expect("SHA256SUMS.txt: valid", ot_sign_check(sums, n, sig, 64, pk), OT_SIG_OK);

    sums[3] ^= 1;               /* one bit of the zip's SHA-256 */
    expect("SHA256SUMS.txt altered (hash): invalid", ot_sign_check(sums, n, sig, 64, pk), OT_SIG_INVALID);
    sums[3] ^= 1;
    sums[n - 10] = '2';         /* the zip's name (another version) */
    expect("SHA256SUMS.txt altered (name): invalid", ot_sign_check(sums, n, sig, 64, pk), OT_SIG_INVALID);
    sums[n - 10] = '3';
    expect("SHA256SUMS.txt truncated: invalid", ot_sign_check(sums, n - 1, sig, 64, pk), OT_SIG_INVALID);
    memcpy(bad, sig, 64);
    bad[0] ^= 0x80;
    expect("signature altered (R): invalid", ot_sign_check(sums, n, bad, 64, pk), OT_SIG_INVALID);
    memcpy(bad, sig, 64);
    bad[63] ^= 0x01;
    expect("signature altered (S): invalid", ot_sign_check(sums, n, bad, 64, pk), OT_SIG_INVALID);
    memset(bad, 0, 64);
    expect("signature all zero: invalid", ot_sign_check(sums, n, bad, 64, pk), OT_SIG_INVALID);
    expect("signature absent", ot_sign_check(sums, n, NULL, 0, pk), OT_SIG_ABSENT);
    expect("signature empty (0 bytes): malformed", ot_sign_check(sums, n, sig, 0, pk), OT_SIG_MALFORMED);
    expect("signature 63 bytes: malformed", ot_sign_check(sums, n, sig, 63, pk), OT_SIG_MALFORMED);
    expect("signature 65 bytes: malformed", ot_sign_check(sums, n, sig, 65, pk), OT_SIG_MALFORMED);
    expect("signature as 128 hex digits: malformed", ot_sign_check(sums, n, sums, 128, pk), OT_SIG_MALFORMED);

    static const uint8_t zero[32];
    expect("no key built in: not checked", ot_sign_check(sums, n, sig, 64, zero), OT_SIG_NO_KEY);
    /* The launcher as shipped today: the key is still all zero. */
    printf("built-in key: %s\n", ot_sign_key_set(ot_release_key) ? "set" : "not set yet (SHA-256 only)");

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
