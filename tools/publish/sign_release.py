#!/usr/bin/env python3
"""The release signature -- SHA256SUMS.txt.sig.

The launcher (port/launcher/app/ot_sign.c) installs an update only when the
release's SHA256SUMS.txt carries a valid Ed25519 signature (RFC 8032, 64 raw
bytes, detached) made with the project's key, whose PUBLIC half is built in
(port/launcher/app/ot_release_key.h).

    sign_release.py sign   --key <private key> <SHA256SUMS.txt>
                           writes <SHA256SUMS.txt>.sig beside it, then checks
                           it against the key built into the launcher
    sign_release.py verify [--pub <64 hex>] <SHA256SUMS.txt> [<.sig>]
                           checks a signature (default: the built-in key)
    sign_release.py pubkey --key <private key>
                           prints the public key, as hex and as the lines of
                           ot_release_key.h

The private key is a PEM file (PKCS#8, "openssl genpkey -algorithm ed25519",
optionally with a passphrase, asked for) kept OUTSIDE any git work tree: the
script refuses a key inside one. It is read, used and dropped; nothing of it
is written anywhere. Needs the Python package "cryptography".
"""
import argparse
import getpass
import os
import re
import stat
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
KEY_HEADER = os.path.join(REPO, "port", "launcher", "app", "ot_release_key.h")

try:
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey
except ImportError:
    sys.exit("needs the Python package 'cryptography' (pip install cryptography)")


def in_git_tree(path):
    """True when path is inside a git work tree (any repository)."""
    d = os.path.dirname(os.path.realpath(path))
    try:
        r = subprocess.run(["git", "-C", d, "rev-parse", "--is-inside-work-tree"],
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    except OSError:
        return False
    return r.returncode == 0 and r.stdout.strip() == "true"


def load_private(path):
    if not os.path.isfile(path):
        sys.exit("no key file: %s" % path)
    if in_git_tree(path):
        sys.exit("refused: %s is inside a git work tree; keep the private key outside any repository" % path)
    if os.name == "posix" and os.stat(path).st_mode & (stat.S_IRWXG | stat.S_IRWXO):
        print("WARNING: %s can be read by other users (chmod 600)" % path, file=sys.stderr)
    with open(path, "rb") as f:
        data = f.read()
    password = None
    if b"ENCRYPTED" in data:
        password = getpass.getpass("passphrase of %s: " % path).encode()
    key = serialization.load_pem_private_key(data, password=password)
    if not isinstance(key, Ed25519PrivateKey):
        sys.exit("%s is not an Ed25519 private key" % path)
    return key


def raw_public(pub):
    return pub.public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)


def built_in_key():
    """The 32 bytes of ot_release_key.h; None while all zero (no key yet)."""
    text = open(KEY_HEADER, encoding="utf-8").read()
    body = text.split("#define OT_RELEASE_KEY_BYTES", 1)[1]
    b = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body)[:32])
    if len(b) != 32:
        sys.exit("cannot read the key of %s" % KEY_HEADER)
    return None if b == bytes(32) else b


def check(sums, sig_path, pub_bytes):
    """0 valid, 1 invalid or absent; prints why."""
    if not os.path.isfile(sig_path):
        print("%s: no signature (%s)" % (sums, sig_path))
        return 1
    msg = open(sums, "rb").read()
    sig = open(sig_path, "rb").read()
    if len(sig) != 64:
        print("%s: not a 64-byte Ed25519 signature (%d bytes)" % (sig_path, len(sig)))
        return 1
    try:
        Ed25519PublicKey.from_public_bytes(pub_bytes).verify(sig, msg)
    except InvalidSignature:
        print("%s: signature INVALID for key %s" % (sums, pub_bytes.hex()))
        return 1
    print("%s: signature valid (key %s)" % (sums, pub_bytes.hex()))
    return 0


def cmd_sign(a):
    key = load_private(a.key)
    pub = raw_public(key.public_key())
    builtin = built_in_key()
    if builtin is None:
        print("WARNING: the launcher holds no key yet (ot_release_key.h all zero): it will not check this signature",
              file=sys.stderr)
    elif builtin != pub and not a.any_key:
        sys.exit("refused: this key (%s) is not the one built into the launcher (%s)" % (pub.hex(), builtin.hex()))
    msg = open(a.sums, "rb").read()
    if not re.match(rb"^[0-9a-f]{64} [ *]\S", msg):
        sys.exit("%s does not look like a SHA256SUMS.txt" % a.sums)
    out = a.sums + ".sig"
    with open(out, "wb") as f:
        f.write(key.sign(msg))
    del key
    print("signed: %s" % out)
    return check(a.sums, out, pub)


def cmd_verify(a):
    if a.pub:
        if not re.fullmatch(r"[0-9a-fA-F]{64}", a.pub):
            sys.exit("--pub: 64 hex digits")
        pub = bytes.fromhex(a.pub)
    else:
        pub = built_in_key()
        if pub is None:
            sys.exit("the launcher holds no key yet (ot_release_key.h): give --pub")
    return check(a.sums, a.sig or a.sums + ".sig", pub)


def cmd_pubkey(a):
    pub = raw_public(load_private(a.key).public_key())
    print(pub.hex())
    print("#define OT_RELEASE_KEY_BYTES \\")
    for i in range(0, 32, 8):
        print("    " + ", ".join("0x%02x" % c for c in pub[i:i + 8]) + (", \\" if i < 24 else ""))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("sign")
    s.add_argument("--key", required=True)
    s.add_argument("--any-key", action="store_true", help="tests: sign with a key other than the built-in one")
    s.add_argument("sums")
    v = sub.add_parser("verify")
    v.add_argument("--pub")
    v.add_argument("sums")
    v.add_argument("sig", nargs="?")
    p = sub.add_parser("pubkey")
    p.add_argument("--key", required=True)
    a = ap.parse_args(argv)
    return {"sign": cmd_sign, "verify": cmd_verify, "pubkey": cmd_pubkey}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
