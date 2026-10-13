/*
 * ot_sign.c -- see ot_sign.h.
 */
#include "ot_sign.h"
#include "ot_release_key.h"

#include "optional/monocypher-ed25519.h"

#include <string.h>

const uint8_t ot_release_key[OT_SIGN_KEY_SIZE] = { OT_RELEASE_KEY_BYTES };

int ot_sign_key_set(const uint8_t key[OT_SIGN_KEY_SIZE])
{
    uint8_t any = 0;
    for (int i = 0; i < OT_SIGN_KEY_SIZE; i++) any |= key[i];
    return any != 0;
}

OtSigResult ot_sign_check(const void *msg, size_t msg_n, const void *sig, size_t sig_n,
                          const uint8_t key[OT_SIGN_KEY_SIZE])
{
    if (!ot_sign_key_set(key)) return OT_SIG_NO_KEY;
    if (!sig) return OT_SIG_ABSENT;
    if (sig_n != OT_SIGN_SIG_SIZE) return OT_SIG_MALFORMED;
    /* crypto_ed25519_check: 0 when valid, -1 otherwise (RFC 8032, cofactorless). */
    return crypto_ed25519_check((const uint8_t *)sig, key, (const uint8_t *)msg, msg_n) == 0
         ? OT_SIG_OK : OT_SIG_INVALID;
}

const char *ot_sign_text(OtSigResult r)
{
    switch (r) {
    case OT_SIG_OK:        return "signature valid";
    case OT_SIG_NO_KEY:    return "no release key built in (SHA-256 only)";
    case OT_SIG_ABSENT:    return "no SHA256SUMS.txt.sig in the release";
    case OT_SIG_MALFORMED: return "SHA256SUMS.txt.sig is not a 64-byte Ed25519 signature";
    case OT_SIG_INVALID:   return "SHA256SUMS.txt.sig does not match SHA256SUMS.txt";
    }
    return "?";
}

/* The length of the x.y.z number at the start of v when what follows it is
 * nothing, "-..." or "+..." (the rule of ot_version_parse, ot_update.cpp);
 * 0 otherwise. */
static size_t version_number_len(const char *v)
{
    size_t p = 0;
    for (int i = 0; i < 3; i++) {
        if (v[p] < '0' || v[p] > '9') return 0;
        while (v[p] >= '0' && v[p] <= '9') p++;
        if (i < 2) {
            if (v[p] != '.') return 0;
            p++;
        }
    }
    return (v[p] == 0 || v[p] == '-' || v[p] == '+') ? p : 0;
}

int ot_sign_zip_is_version(const char *zip_name, const char *version)
{
    if (!version || !version[0]) return 1;
    if (!zip_name) return 0;
    size_t n = version_number_len(version);
    if (n == 0) n = strlen(version);
    size_t zn = strlen(zip_name);
    /* "-" + version[0..n) + "-" anywhere in zip_name */
    for (size_t i = 0; i + n + 2 <= zn; i++)
        if (zip_name[i] == '-' && zip_name[i + n + 1] == '-' && memcmp(zip_name + i + 1, version, n) == 0)
            return 1;
    return 0;
}
