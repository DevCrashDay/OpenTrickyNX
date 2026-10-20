#!/usr/bin/env python3
"""EA RefPack (QFS) -- the compression of every member of an Xbox course archive.

Header: two flag bytes "10 FB" (0x8000 = 4-byte sizes, 0x0100 = a
compressed-size field first), then the unpacked size, big-endian. Then
commands; each copies 0-3 (or 4-112) literal bytes, then an earlier run:

    0ooLLLpp oooooooo                     run 3-10,   offset up to 1024
    10LLLLLL ppoooooo oooooooo            run 4-67,   offset up to 16384
    110oLLpp oooooooo oooooooo LLLLLLLL   run 5-1028, offset up to 131072
    111ppppp                              4-112 literals, no run
    111111pp                              0-3 literals, then the end

    refpack.py d <in> <out>     decompress
    refpack.py c <in> <out>     compress
"""
import sys

WINDOW = 131072
MAX_RUN = 1028
CHAIN = 64          # candidates tried per position: speed against size


def is_refpack(data):
    return len(data) >= 5 and data[1] == 0xFB and (data[0] & 0x3E) == 0x10


def _header(data):
    flags = data[0] << 8 | data[1]
    n = 4 if flags & 0x8000 else 3
    p = 2 + (n if flags & 0x0100 else 0)
    return int.from_bytes(data[p:p + n], "big"), p + n


def unpacked_size(data):
    return _header(data)[0]


def decompress(data):
    size, p = _header(data)
    out = bytearray()
    while p < len(data):
        b0 = data[p]
        if b0 < 0x80:
            b1 = data[p + 1]
            p += 2
            lit, run, off = b0 & 3, ((b0 >> 2) & 7) + 3, ((b0 & 0x60) << 3) + b1 + 1
        elif b0 < 0xC0:
            b1, b2 = data[p + 1], data[p + 2]
            p += 3
            lit, run, off = b1 >> 6, (b0 & 0x3F) + 4, ((b1 & 0x3F) << 8) + b2 + 1
        elif b0 < 0xE0:
            b1, b2, b3 = data[p + 1], data[p + 2], data[p + 3]
            p += 4
            lit, run, off = b0 & 3, ((b0 & 0x0C) << 6) + b3 + 5, ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1
        elif b0 < 0xFC:
            p += 1
            lit, run, off = ((b0 & 0x1F) + 1) * 4, 0, 0
        else:
            p += 1
            lit, run, off = b0 & 3, 0, 0
        out += data[p:p + lit]
        p += lit
        if run:
            if off > len(out):
                raise ValueError("RefPack run reaches before the start")
            if off >= run:
                out += out[-off:len(out) - off + run]
            else:
                for _ in range(run):
                    out.append(out[-off])
        if b0 >= 0xFC:
            break
    if len(out) != size:
        raise ValueError(f"RefPack: unpacked {len(out)} bytes, header says {size}")
    return bytes(out)


def _literals(out, data, start, end):
    """Emit data[start:end] as 4-112 byte literal blocks; returns the 0-3 left over."""
    while end - start >= 4:
        n = min(112, (end - start) // 4 * 4)
        out.append(0xE0 | (n // 4 - 1))
        out += data[start:start + n]
        start += n
    return start


def compress(data):
    """Greedy LZ with hash chains; output decompresses to data exactly."""
    n = len(data)
    if n > 0xFFFFFF:
        raise ValueError("member too large for a 3-byte RefPack size")
    out = bytearray(b"\x10\xFB" + n.to_bytes(3, "big"))
    head, prev = {}, [0] * n
    i = lit = 0
    while i < n:
        best_len = best_off = 0
        if i + 3 <= n:
            key = data[i:i + 3]
            j = head.get(key, -1)
            tries = CHAIN
            limit = min(MAX_RUN, n - i)
            while j >= 0 and i - j <= WINDOW and tries:
                if data[j + best_len:j + best_len + 1] == data[i + best_len:i + best_len + 1]:
                    k = 3
                    while k < limit and data[j + k] == data[i + k]:
                        k += 1
                    if k > best_len:
                        best_len, best_off = k, i - j
                        if k == limit:
                            break
                j = prev[j]
                tries -= 1
        # a run must beat its own command size; shorter or far runs are literals
        if best_len >= 3 and not (best_off > 1024 and best_len < 4) and not (best_off > 16384 and best_len < 5):
            lit = _literals(out, data, lit, i)
            nlit, off = i - lit, best_off - 1
            if best_len <= 10 and best_off <= 1024:
                out += bytes((((off >> 8) & 3) << 5 | (best_len - 3) << 2 | nlit, off & 0xFF))
            elif best_len <= 67 and best_off <= 16384:
                out += bytes((0x80 | (best_len - 4), nlit << 6 | (off >> 8), off & 0xFF))
            else:
                L = best_len - 5
                out += bytes((0xC0 | ((off >> 16) & 1) << 4 | ((L >> 8) & 3) << 2 | nlit, (off >> 8) & 0xFF, off & 0xFF, L & 0xFF))
            out += data[lit:i]
            for k in range(i, i + best_len):
                if k + 3 <= n:
                    key = data[k:k + 3]
                    prev[k] = head.get(key, -1)
                    head[key] = k
            i += best_len
            lit = i
        else:
            key = data[i:i + 3]
            prev[i] = head.get(key, -1)
            head[key] = i
            i += 1
    lit = _literals(out, data, lit, n)
    out.append(0xFC | (n - lit))
    out += data[lit:n]
    return bytes(out)


if __name__ == "__main__":
    if len(sys.argv) != 4 or sys.argv[1] not in ("c", "d"):
        sys.exit(__doc__)
    with open(sys.argv[2], "rb") as f:
        src = f.read()
    with open(sys.argv[3], "wb") as f:
        f.write(compress(src) if sys.argv[1] == "c" else decompress(src))
