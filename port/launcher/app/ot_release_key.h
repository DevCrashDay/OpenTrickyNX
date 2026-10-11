/*
 * ot_release_key.h -- the project's Ed25519 public key.
 *
 * Only the PUBLIC key goes here. All zero means "no key yet": the updater
 * then keeps the SHA-256 check alone (as 0.1.1 does). Once the key exists:
 *
 *   python3 tools/publish/sign_release.py pubkey --key <private key, outside git>
 *
 * prints the 32 bytes to paste below. From the launcher built with them,
 * every release must carry SHA256SUMS.txt.sig (tools/publish/README.md).
 */
#ifndef OT_RELEASE_KEY_H
#define OT_RELEASE_KEY_H

#define OT_RELEASE_KEY_BYTES \
    0x77, 0xe4, 0x47, 0xed, 0xeb, 0x08, 0xa3, 0x20, \
    0x6b, 0xe2, 0xc5, 0x9d, 0x1b, 0x09, 0x01, 0x2f, \
    0x1f, 0x88, 0xc3, 0x77, 0xd0, 0x77, 0x63, 0xfc, \
    0x77, 0x8b, 0x44, 0x47, 0xcb, 0x41, 0x61, 0x33

#endif
