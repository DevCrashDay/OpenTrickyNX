#!/usr/bin/env python3
"""Xbox course archives -- data/models/<course>.big (see docs/xbox-level-format.md).

    course_big.py list   <course.big>
    course_big.py unpack <course.big> <folder>
                         writes each member unpacked (RefPack undone) and
                         manifest.json, the member order and names
    course_big.py pack   <folder> <course.big>
                         the reverse: RefPack-compresses each member listed in
                         the folder's manifest.json and writes the archive

The archive is EA's C0FB layout: big-endian "C0 FB", header size less 4 (u16),
member count (u16), then per member a 3-byte offset, a 3-byte size and a
NUL-terminated name; members start on 0x80-byte boundaries. Each member is
RefPack-compressed ("10 FB", 3-byte unpacked size).

Works on the owner's own disc files only; nothing of the game is stored in
the repository.
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import refpack  # noqa: E402

MAGIC = b"\xC0\xFB"
ALIGN = 0x80


def read_big(data):
    """[(name, stored bytes)] in archive order."""
    if data[:2] != MAGIC:
        raise ValueError("not a C0FB archive")
    count = int.from_bytes(data[4:6], "big")
    p, out = 6, []
    for _ in range(count):
        off = int.from_bytes(data[p:p + 3], "big")
        size = int.from_bytes(data[p + 3:p + 6], "big")
        end = data.index(b"\0", p + 6)
        out.append((data[p + 6:end].decode("ascii"), data[off:off + size]))
        p = end + 1
    return out


def write_big(members):
    """members: [(name, stored bytes)] -> archive bytes, in that order."""
    names = [n.encode("ascii") + b"\0" for n, _ in members]
    hsize = 6 + sum(6 + len(n) for n in names)
    if hsize > 0xFFFF or len(members) > 0xFFFF:
        raise ValueError("too many members for a C0FB header")
    pos = (hsize + ALIGN - 1) // ALIGN * ALIGN
    # the size field counts the header after its first four bytes (retail: 13 members, 369 for 373)
    head = bytearray(MAGIC + (hsize - 4).to_bytes(2, "big") + len(members).to_bytes(2, "big"))
    body = bytearray()
    for (name, blob), raw in zip(members, names):
        if pos + len(blob) > 0xFFFFFF:
            raise ValueError("archive outgrows the 3-byte offsets of a C0FB header")
        head += pos.to_bytes(3, "big") + len(blob).to_bytes(3, "big") + raw
        body += b"\0" * (pos - len(body) - ALIGN * ((hsize + ALIGN - 1) // ALIGN)) + blob
        pos = (pos + len(blob) + ALIGN - 1) // ALIGN * ALIGN
    head += b"\0" * ((hsize + ALIGN - 1) // ALIGN * ALIGN - hsize)
    return bytes(head + body)       # retail archives end on the last member, unpadded


def cmd_list(args):
    with open(args.big, "rb") as f:
        for name, blob in read_big(f.read()):
            kind = "refpack" if refpack.is_refpack(blob) else "stored"
            size = refpack.unpacked_size(blob) if kind == "refpack" else len(blob)
            print(f"{len(blob):9d} {size:9d} {kind:7s} {name}")


def cmd_unpack(args):
    with open(args.big, "rb") as f:
        members = read_big(f.read())
    os.makedirs(args.folder, exist_ok=True)
    manifest = []
    for name, blob in members:
        base = os.path.basename(name)
        packed = refpack.is_refpack(blob)
        with open(os.path.join(args.folder, base), "wb") as f:
            f.write(refpack.decompress(blob) if packed else blob)
        manifest.append({"name": name, "file": base, "refpack": packed})
    with open(os.path.join(args.folder, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(f"{len(members)} members -> {args.folder}")


def cmd_pack(args):
    with open(os.path.join(args.folder, "manifest.json"), encoding="utf-8") as f:
        manifest = json.load(f)
    members = []
    for m in manifest:
        with open(os.path.join(args.folder, m["file"]), "rb") as f:
            raw = f.read()
        members.append((m["name"], refpack.compress(raw) if m.get("refpack", True) else raw))
    with open(args.big, "wb") as f:
        f.write(write_big(members))
    print(f"{len(members)} members -> {args.big}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list")
    p.add_argument("big")
    p.set_defaults(fn=cmd_list)
    p = sub.add_parser("unpack")
    p.add_argument("big")
    p.add_argument("folder")
    p.set_defaults(fn=cmd_unpack)
    p = sub.add_parser("pack")
    p.add_argument("folder")
    p.add_argument("big")
    p.set_defaults(fn=cmd_pack)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
