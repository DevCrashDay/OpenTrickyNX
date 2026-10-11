/*
 * ot_sign.c -- see ot_sign.h.
 */
#include "ot_sign.h"
#include "ot_release_key.h"

#include "optional/monocypher-ed25519.h"

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
