#!/usr/bin/env python3
"""Test of sign_release.py -- valid, altered and absent signature.

    python3 tools/publish/test_sign_release.py [<ot_sign_test binary>]

Signs a sample SHA256SUMS.txt with the public test key of RFC 8032 (section
7.1, TEST 1), never a key of the project, written to a temporary folder
outside the repository and removed after. Given the launcher's host test
(port/launcher/tools/ot_sign_test.c, built), the same files are checked by
the launcher's code too.
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sign_release as sr  # noqa: E402

from cryptography.hazmat.primitives import serialization  # noqa: E402
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey  # noqa: E402

RFC_SEED = bytes.fromhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
RFC_PUB = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"
fails = 0


def expect(what, got, want):
    global fails
    ok = got == want
    print("%-46s %s" % (what, "ok" if ok else "FAIL (%r, wanted %r)" % (got, want)))
    fails += not ok


def c_check(binary, sums, sig):
    if not binary:
        return None
    return subprocess.run([binary, sums, sig, RFC_PUB], stdout=subprocess.DEVNULL).returncode


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else None
    tmp = tempfile.mkdtemp(prefix="sign-test-")
    try:
        if sr.in_git_tree(os.path.join(tmp, "x")):
            sys.exit("the temporary folder is inside a git work tree: set TMPDIR elsewhere")
        key = os.path.join(tmp, "test-rfc8032.pem")
        with open(key, "wb") as f:
            f.write(Ed25519PrivateKey.from_private_bytes(RFC_SEED).private_bytes(
                serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
        os.chmod(key, 0o600)
        sums = os.path.join(tmp, "SHA256SUMS.txt")
        with open(sums, "w", encoding="ascii", newline="\n") as f:
            f.write("%s  OpenTricky-0.1.3-win64.zip\n" % ("ab" * 32))
        sig = sums + ".sig"

        expect("pubkey of the RFC seed", sr.raw_public(sr.load_private(key).public_key()).hex(), RFC_PUB)
        expect("sign (test key, --any-key)", sr.main(["sign", "--key", key, "--any-key", sums]), 0)
        expect("signature is 64 bytes", os.path.getsize(sig), 64)
        expect("verify: valid", sr.main(["verify", "--pub", RFC_PUB, sums]), 0)
        if binary:
            expect("launcher code: valid", c_check(binary, sums, sig), 0)

        original = open(sums, "rb").read()
        with open(sums, "wb") as f:
            f.write(original.replace(b"0.1.3", b"0.1.2"))
        expect("verify: altered SHA256SUMS.txt refused", sr.main(["verify", "--pub", RFC_PUB, sums]), 1)
        if binary:
            expect("launcher code: altered refused", c_check(binary, sums, sig), 1)
        with open(sums, "wb") as f:
            f.write(original)

        good = open(sig, "rb").read()
        with open(sig, "wb") as f:
            f.write(bytes([good[0] ^ 1]) + good[1:])
        expect("verify: altered signature refused", sr.main(["verify", "--pub", RFC_PUB, sums]), 1)
        if binary:
            expect("launcher code: altered signature refused", c_check(binary, sums, sig), 1)

        os.remove(sig)
        expect("verify: absent signature refused", sr.main(["verify", "--pub", RFC_PUB, sums]), 1)
        if binary:
            expect("launcher code: absent signature refused", c_check(binary, sums, sig), 1)

        other = "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c"
        sr.main(["sign", "--key", key, "--any-key", sums])
        expect("verify: another key refused", sr.main(["verify", "--pub", other, sums]), 1)

        # The key inside a git work tree (this repository): refused.
        inside = os.path.join(HERE, ".sign-test-key.pem")
        shutil.copy(key, inside)
        try:
            sr.main(["sign", "--key", inside, "--any-key", sums])
            refused = False
        except SystemExit as e:
            refused = "inside a git work tree" in str(e)
        finally:
            os.remove(inside)
        expect("key inside a git work tree refused", refused, True)

        # While ot_release_key.h is all zero, signing still works (warning);
        # once it holds the key, a different key is refused without --any-key.
        if sr.built_in_key() is None:
            expect("built-in key not set yet: sign warns", sr.main(["sign", "--key", key, sums]), 0)
        else:
            try:
                sr.main(["sign", "--key", key, sums])
                refused = False
            except SystemExit:
                refused = True
            expect("key other than the built-in one refused", refused, True)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("%d FAILED" % fails if fails else "all passed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
