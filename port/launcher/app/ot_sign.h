/*
 * ot_sign.h -- the release signature.
 *
 * Each release carries, beside its zip and SHA256SUMS.txt, the asset
 * SHA256SUMS.txt.sig: the Ed25519 signature (RFC 8032, 64 raw bytes,
 * detached) of SHA256SUMS.txt exactly as uploaded. The public key is built
 * into the launcher (ot_release_key.h); the private key never is, nor is it
 * in git: tools/publish/sign_release.py reads it from a file outside the
 * repository.
 *
 * The updater checks the signature of SHA256SUMS.txt before reading a line
 * of it, so the zip's SHA-256 it then compares comes from a signed list,
 * and nothing of the zip is decoded before that.
 *
 * Plain C99 (Monocypher, port/third_party/monocypher): no SDL, so the host
 * test (port/launcher/tools/ot_sign_test.c) builds it alone.
 */
#ifndef OT_SIGN_H
#define OT_SIGN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OT_SIGN_KEY_SIZE 32
#define OT_SIGN_SIG_SIZE 64

typedef enum {
    OT_SIG_OK = 0,
    OT_SIG_NO_KEY,      /* the launcher holds no key yet (all zero): not checked */
    OT_SIG_ABSENT,      /* no SHA256SUMS.txt.sig */
    OT_SIG_MALFORMED,   /* not 64 bytes */
    OT_SIG_INVALID      /* does not match SHA256SUMS.txt and the key */
} OtSigResult;

/* The project's public key, built in (ot_release_key.h). */
extern const uint8_t ot_release_key[OT_SIGN_KEY_SIZE];

/* 1 when key is not all zero. */
int ot_sign_key_set(const uint8_t key[OT_SIGN_KEY_SIZE]);

/* Checks sig (sig_n bytes, NULL = absent) over msg with key. */
OtSigResult ot_sign_check(const void *msg, size_t msg_n, const void *sig, size_t sig_n,
                          const uint8_t key[OT_SIGN_KEY_SIZE]);

/* A short English text for the log. */
const char *ot_sign_text(OtSigResult r);

#ifdef __cplusplus
}
#endif

#endif
