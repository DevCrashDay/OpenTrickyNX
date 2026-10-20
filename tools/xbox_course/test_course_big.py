#!/usr/bin/env python3
"""Test of refpack.py and course_big.py on made-up data (no game files).

    python3 tools/xbox_course/test_course_big.py [<course.big> ...]

Each RefPack command type is exercised: short, medium and long runs,
overlapping runs, long literal stretches, and an empty member. Given retail
archives, each is also read and written back, which must give the same bytes,
and its members recompressed, which must unpack to the same bytes.
"""
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import course_big as cb  # noqa: E402
import refpack  # noqa: E402

fails = 0


def expect(what, ok):
    global fails
    print(("ok   " if ok else "FAIL ") + what)
    fails += 0 if ok else 1


def samples():
    rng = random.Random(1)
    noise = bytes(rng.randrange(256) for _ in range(5000))
    far = bytes(rng.randrange(256) for _ in range(100000))
    yield "empty", b""
    yield "three bytes", b"abc"
    yield "noise", noise
    yield "one byte repeated", b"\x5A" * 3000
    yield "short runs", b"".join(noise[i:i + 7] + noise[:3] for i in range(0, 2000, 7))
    yield "medium offsets", noise + noise[100:160] + noise[:4000] + noise[2000:2060]
    yield "long offsets", far + far[1000:1900] + far[:20]
    yield "floats", b"".join(int(i * 2654435761 % 4096).to_bytes(4, "little") + b"\0\0\x80\x3F" for i in range(4000))


def main():
    for name, data in samples():
        packed = refpack.compress(data)
        expect(f"refpack {name}: {len(data)} -> {len(packed)}", refpack.decompress(packed) == data)
        expect(f"refpack {name}: header", refpack.is_refpack(packed) and refpack.unpacked_size(packed) == len(data))

    members = [(f"data/models/test{i}.bin", refpack.compress(d)) for i, (_, d) in enumerate(samples())]
    big = cb.write_big(members)
    expect("archive: members read back", cb.read_big(big) == members)
    expect("archive: members on 0x80 boundaries",
           all(big.find(blob) % cb.ALIGN == 0 for _, blob in members if blob))
    expect("archive: ends on the last member", big.endswith(members[-1][1]))

    for path in sys.argv[1:]:
        with open(path, "rb") as f:
            data = f.read()
        got = cb.read_big(data)
        expect(f"{os.path.basename(path)}: written back byte for byte", cb.write_big(got) == data)
        for name, blob in got:
            raw = refpack.decompress(blob)
            expect(f"{os.path.basename(path)}: {name} recompressed", refpack.decompress(refpack.compress(raw)) == raw)

    print("all passed" if not fails else f"{fails} failed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
