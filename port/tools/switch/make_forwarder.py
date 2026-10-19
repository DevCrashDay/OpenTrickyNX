#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""A home-menu entry for OpenTricky on the Switch: an installable .nsp that
starts sdmc:/switch/opentricky/SSX Tricky.nro (a "forwarder").

    make_forwarder.py --keys prod.keys --cover cover.png \
        --nso forwarder.nso --npdm forwarder.npdm --out "OpenTricky.nsp"

--nso/--npdm are the small loader of port/switch/forwarder (nx-hbloader that
starts the game and closes when it returns), built by build_switch_nsp.sh.
--cover is any picture (your box art); it becomes the 256x256 home-menu icon.
--keys is your console's own key file (dumped with Lockpick_RCM); it needs
header_key and key_area_key_application_00 (or the --keygen you pick). Keys,
cover and the .nsp stay on your computer: none of them is in the repository.

The .nsp holds three NCAs: the program (the loader), the control data (name,
icon) and the content meta. They carry no Nintendo signature, so installing
needs a console with custom firmware and signature patches. Pure Python
(AES included) with Pillow for the icon.
"""
import argparse, hashlib, io, os, struct, sys

TITLE_ID = 0x0500535358540000      # must match forwarder.json
MEDIA = 0x200

# --------------------------------------------------------------------- AES-128

def _xt(a):
    return ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else a << 1


def _sbox():
    s = [0] * 256
    p = q = 1
    while True:
        p = p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)
        q ^= q << 1; q ^= q << 2; q ^= q << 4; q &= 0xFF
        if q & 0x80:
            q ^= 0x09
        x = q ^ (q << 1 | q >> 7) ^ (q << 2 | q >> 6) ^ (q << 3 | q >> 5) ^ (q << 4 | q >> 4)
        s[p] = (x ^ 0x63) & 0xFF
        if p == 1:
            break
    s[0] = 0x63
    return s


SBOX = _sbox()
# T-table: column of MixColumns(SubBytes(b)) as a 32-bit word, then rotations
T0 = [(_xt(SBOX[b]) << 24) | (SBOX[b] << 16) | (SBOX[b] << 8) | (_xt(SBOX[b]) ^ SBOX[b])
      for b in range(256)]
T1 = [((t >> 8) | (t << 24)) & 0xFFFFFFFF for t in T0]
T2 = [((t >> 16) | (t << 16)) & 0xFFFFFFFF for t in T0]
T3 = [((t >> 24) | (t << 8)) & 0xFFFFFFFF for t in T0]


class AES128:
    def __init__(self, key):
        assert len(key) == 16
        w = list(struct.unpack('>4I', key))
        rcon = 1
        for i in range(4, 44):
            t = w[i - 1]
            if i % 4 == 0:
                t = ((SBOX[(t >> 16) & 0xFF] << 24) | (SBOX[(t >> 8) & 0xFF] << 16) |
                     (SBOX[t & 0xFF] << 8) | SBOX[t >> 24]) ^ (rcon << 24)
                rcon = _xt(rcon)
            w.append(w[i - 4] ^ t)
        self.rk = w

    def block(self, b):
        rk = self.rk
        s0, s1, s2, s3 = struct.unpack('>4I', b)
        s0 ^= rk[0]; s1 ^= rk[1]; s2 ^= rk[2]; s3 ^= rk[3]
        for r in range(1, 10):
            k = 4 * r
            t0 = T0[s0 >> 24] ^ T1[(s1 >> 16) & 0xFF] ^ T2[(s2 >> 8) & 0xFF] ^ T3[s3 & 0xFF] ^ rk[k]
            t1 = T0[s1 >> 24] ^ T1[(s2 >> 16) & 0xFF] ^ T2[(s3 >> 8) & 0xFF] ^ T3[s0 & 0xFF] ^ rk[k + 1]
            t2 = T0[s2 >> 24] ^ T1[(s3 >> 16) & 0xFF] ^ T2[(s0 >> 8) & 0xFF] ^ T3[s1 & 0xFF] ^ rk[k + 2]
            t3 = T0[s3 >> 24] ^ T1[(s0 >> 16) & 0xFF] ^ T2[(s1 >> 8) & 0xFF] ^ T3[s2 & 0xFF] ^ rk[k + 3]
            s0, s1, s2, s3 = t0, t1, t2, t3
        S = SBOX
        out = []
        for i, (a, b_, c, d) in enumerate(((s0, s1, s2, s3), (s1, s2, s3, s0),
                                           (s2, s3, s0, s1), (s3, s0, s1, s2))):
            out.append(((S[a >> 24] << 24) | (S[(b_ >> 16) & 0xFF] << 16) |
                        (S[(c >> 8) & 0xFF] << 8) | S[d & 0xFF]) ^ rk[40 + i])
        return struct.pack('>4I', *out)


def ecb(key, data):
    a = AES128(key)
    return b''.join(a.block(data[i:i + 16]) for i in range(0, len(data), 16))


def ctr(key, iv_hi, offset, data):
    """AES-CTR as NCA sections use it: counter = iv_hi (8 bytes) ||
    big-endian (absolute offset / 16)."""
    a = AES128(key)
    out = bytearray(len(data))
    n = offset >> 4
    for i in range(0, len(data), 16):
        ks = a.block(iv_hi + struct.pack('>Q', n))
        chunk = data[i:i + 16]
        out[i:i + len(chunk)] = bytes(x ^ y for x, y in zip(chunk, ks))
        n += 1
    return bytes(out)


def _gf_double(t):
    v = int.from_bytes(t, 'little') << 1
    if v >> 128:
        v = (v & ((1 << 128) - 1)) ^ 0x87
    return v.to_bytes(16, 'little')


def xts_nintendo(key32, data, sector_size=0x200):
    """AES-XTS with Nintendo's tweak: the sector number, big-endian."""
    k1, k2 = AES128(key32[:16]), AES128(key32[16:])
    out = bytearray()
    for s in range(len(data) // sector_size):
        t = k2.block(s.to_bytes(16, 'big'))
        sec = data[s * sector_size:(s + 1) * sector_size]
        for i in range(0, sector_size, 16):
            x = bytes(p ^ q for p, q in zip(sec[i:i + 16], t))
            out += bytes(p ^ q for p, q in zip(k1.block(x), t))
            t = _gf_double(t)
    return bytes(out)

# ------------------------------------------------------------------ containers

def align(n, a):
    return (n + a - 1) // a * a


def pfs0(files):
    """files: list of (name, bytes). Header padded to 0x20 as Nintendo's."""
    names = b''
    offs = []
    for n, _ in files:
        offs.append(len(names))
        names += n.encode() + b'\0'
    hdr_len = 0x10 + 0x18 * len(files) + len(names)
    names += b'\0' * (align(hdr_len, 0x20) - hdr_len)
    out = struct.pack('<4sIII', b'PFS0', len(files), len(names), 0)
    pos = 0
    for (n, d), so in zip(files, offs):
        out += struct.pack('<QQII', pos, len(d), so, 0)
        pos += len(d)
    out += names
    for _, d in files:
        out += d
    return out


def _romfs_hash(parent, name):
    h = parent ^ 123456789
    for c in name:
        h = ((h >> 5) | (h << 27)) & 0xFFFFFFFF
        h ^= c
    return h


def romfs(files):
    """A RomFS with the files in its root directory."""
    files = sorted(files)
    data = b''
    placed = []
    for n, d in files:
        data += b'\0' * (align(len(data), 0x10) - len(data))
        placed.append((n.encode(), len(data), len(d)))
        data += d
    nb = 3 if len(files) < 3 else (len(files) | 1)
    fht = [0xFFFFFFFF] * nb
    fmeta = b''
    for i, (n, off, size) in enumerate(placed):
        me = len(fmeta)
        h = _romfs_hash(0, n) % nb
        sib = 0xFFFFFFFF if i == len(placed) - 1 else me + align(0x20 + len(n), 4)
        fmeta += struct.pack('<IIQQII', 0, sib, off, size, fht[h], len(n))
        fmeta += n + b'\0' * (align(len(n), 4) - len(n))
        fht[h] = me
    dht = [0xFFFFFFFF] * 3
    dht[_romfs_hash(0, b'') % 3] = 0
    dmeta = struct.pack('<IIIIII', 0, 0xFFFFFFFF, 0xFFFFFFFF,
                        0 if placed else 0xFFFFFFFF, 0xFFFFFFFF, 0)
    data_off = 0x200
    t = align(data_off + len(data), 4)
    tables = []
    for blob in (struct.pack('<3I', *dht), dmeta, struct.pack('<%dI' % nb, *fht), fmeta):
        tables.append((t, len(blob), blob))
        t += len(blob)
    hdr = struct.pack('<Q', 0x50)
    for o, l, _ in tables:
        hdr += struct.pack('<QQ', o, l)
    hdr += struct.pack('<Q', data_off)
    out = bytearray(hdr + b'\0' * (data_off - len(hdr)) + data)
    out += b'\0' * (tables[0][0] - len(out))
    for _, _, blob in tables:
        out += blob
    return bytes(out)

# ------------------------------------------------------------------------- NCA

def section_pfs0(data, block=0x1000):
    """(section bytes, fs-header hash data): HierarchicalSha256 over a PFS0."""
    hashes = b''.join(hashlib.sha256(data[i:i + block]).digest()
                      for i in range(0, len(data), block))
    data_off = align(len(hashes), MEDIA)
    body = hashes + b'\0' * (data_off - len(hashes)) + data
    hd = hashlib.sha256(hashes).digest() + struct.pack('<II', block, 2)
    hd += struct.pack('<QQQQ', 0, len(hashes), data_off, len(data))
    return body, 1, 2, hd


def section_romfs(data, log2=14):
    """(section bytes, ...): IVFC (six levels, the last is the RomFS)."""
    bs = 1 << log2
    levels = [data]
    for _ in range(5):
        cur = levels[0]
        levels.insert(0, b''.join(
            hashlib.sha256(cur[i:i + bs].ljust(bs, b'\0')).digest()
            for i in range(0, max(len(cur), 1), bs)))
    body = b''
    info = b''
    for lv in levels:
        body += b'\0' * (align(len(body), bs) - len(body))
        info += struct.pack('<QQII', len(body), len(lv), log2, 0)
        body += lv
    master = hashlib.sha256(levels[0].ljust(bs, b'\0')).digest()
    hd = struct.pack('<4sIII', b'IVFC', 0x20000, 0x20, 7) + info + b'\0' * 0x20 + master
    return body, 0, 3, hd


def nca(keys, keygen, content_type, sections, title_id=TITLE_ID):
    """content_type: 0 program, 1 meta, 2 control. sections: from section_*."""
    key_ctr = hashlib.sha256(b'OpenTricky forwarder %d' % content_type).digest()[:16]
    kak = keys['key_area_key_application_%02x' % keygen]
    body = bytearray()
    entries = b''
    fs_headers = []
    pos = 0xC00
    for idx, (sec, fstype, hashtype, hashdata) in enumerate(sections):
        sec = sec + b'\0' * (align(len(sec), MEDIA) - len(sec))
        ctr_hi = struct.pack('<II', 0, idx + 1)    # generation, secure value
        fh = struct.pack('<HBBBB2x', 2, fstype, hashtype, 3, 0)
        fh += hashdata.ljust(0xF8, b'\0')
        fh += b'\0' * 0x40 + ctr_hi
        fh = fh.ljust(0x200, b'\0')
        fs_headers.append(fh)
        body += ctr(key_ctr, ctr_hi[::-1], pos, sec)
        entries += struct.pack('<II8x', pos // MEDIA, (pos + len(sec)) // MEDIA)
        pos += len(sec)
    size = 0xC00 + len(body)
    h = bytearray(0x400)
    h[0x200:0x204] = b'NCA3'
    h[0x204] = 0                       # downloaded, not a game card
    h[0x205] = content_type
    h[0x206] = keygen if keygen <= 2 else 2
    h[0x207] = 0                       # key area: application keys
    struct.pack_into('<QQIIB', h, 0x208, size, title_id, 0, 0x000C1100,
                     keygen if keygen > 2 else 0)
    h[0x240:0x240 + len(entries)] = entries
    for i, fh in enumerate(fs_headers):
        h[0x280 + 0x20 * i:0x2A0 + 0x20 * i] = hashlib.sha256(fh).digest()
    h[0x300:0x340] = ecb(kak, b'\0' * 0x20 + key_ctr + b'\0' * 0x10)
    plain = bytes(h) + b''.join(fs_headers) + b'\0' * (0x200 * (4 - len(fs_headers)))
    return xts_nintendo(keys['header_key'], plain) + bytes(body)


def content_id(blob):
    return hashlib.sha256(blob).digest()[:16]


def cnmt(contents, title_id=TITLE_ID):
    """contents: list of (nca bytes, cnmt content type: 1 program, 3 control)."""
    out = struct.pack('<QIBxHHHB3xI4x', title_id, 0, 0x80, 0x10, len(contents), 0, 0, 0)
    out += struct.pack('<QII', title_id + 0x800, 0, 0)
    for blob, ctype in contents:
        out += hashlib.sha256(blob).digest() + content_id(blob)
        out += len(blob).to_bytes(6, 'little') + bytes([ctype, 0])
    return out + b'\0' * 0x20

# ----------------------------------------------------------------- NACP, icon

def nacp(name, publisher, version):
    n = bytearray(0x4000)
    for i in range(16):
        n[0x300 * i:0x300 * i + len(name)] = name
        n[0x300 * i + 0x200:0x300 * i + 0x200 + len(publisher)] = publisher
    struct.pack_into('<I', n, 0x302C, 0xFFFF)      # all languages use the entry
    n[0x3034] = 0                                  # screenshots allowed
    n[0x3035] = 2                                  # video capture: automatic
    struct.pack_into('<Q', n, 0x3038, TITLE_ID)    # presence group
    n[0x3040:0x3060] = b'\xff' * 0x20              # no age rating
    n[0x3060:0x3060 + len(version)] = version
    struct.pack_into('<Q', n, 0x3078, TITLE_ID)    # save data owner
    struct.pack_into('<Q', n, 0x30B0, TITLE_ID)    # local communication id
    n[0x30F0] = 2                                  # logo type: Nintendo
    n[0x30F1] = 0                                  # logo handling: auto
    return bytes(n)


def icon_jpeg(path):
    from PIL import Image, ImageFilter, ImageEnhance
    src = Image.open(path).convert('RGB')
    w, h = src.size
    # background: the cover filling the square, blurred and darkened
    s = 256 / min(w, h)
    bg = src.resize((max(256, round(w * s)), max(256, round(h * s))), Image.LANCZOS)
    l, t = (bg.width - 256) // 2, (bg.height - 256) // 2
    bg = bg.crop((l, t, l + 256, t + 256)).filter(ImageFilter.GaussianBlur(10))
    bg = ImageEnhance.Brightness(bg).enhance(0.55)
    # the whole cover on top, as large as it fits, with a soft shadow
    s = 256 / max(w, h)
    fw, fh = max(1, round(w * s)), max(1, round(h * s))
    fg = src.resize((fw, fh), Image.LANCZOS)
    x, y = (256 - fw) // 2, (256 - fh) // 2
    if fw < 256 or fh < 256:
        sh = Image.new('L', (256, 256), 0)
        sh.paste(150, (x + 3, y + 3, x + fw + 3, y + fh + 3))
        sh = sh.filter(ImageFilter.GaussianBlur(5))
        bg.paste((0, 0, 0), (0, 0), sh)
    bg.paste(fg, (x, y))
    out = io.BytesIO()
    q = 92
    while True:
        out.seek(0); out.truncate()
        bg.save(out, 'JPEG', quality=q, optimize=True, progressive=False, subsampling=0)
        if out.tell() <= 0x20000 or q <= 50:
            return out.getvalue()
        q -= 6

# ------------------------------------------------------------------------ main

def read_keys(path):
    keys = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if '=' in line:
                k, v = line.split('=', 1)
                try:
                    keys[k.strip().lower()] = bytes.fromhex(v.strip())
                except ValueError:
                    pass
    return keys


def build(keys, keygen, nso, npdm, icon, name, publisher, version):
    program = nca(keys, keygen, 0, [section_pfs0(pfs0([('main', nso), ('main.npdm', npdm)]))])
    control = nca(keys, keygen, 2, [section_romfs(romfs([
        ('control.nacp', nacp(name, publisher, version)),
        ('icon_AmericanEnglish.dat', icon)]))])
    meta_name = 'Application_%016x.cnmt' % TITLE_ID
    meta = nca(keys, keygen, 1, [section_pfs0(pfs0([(meta_name, cnmt([(program, 1), (control, 3)]))]))])
    return pfs0([(content_id(program).hex() + '.nca', program),
                 (content_id(control).hex() + '.nca', control),
                 (content_id(meta).hex() + '.cnmt.nca', meta)])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--keys', required=True, help='your prod.keys')
    ap.add_argument('--cover', required=True, help='picture for the icon (png, jpg)')
    ap.add_argument('--nso', required=True)
    ap.add_argument('--npdm', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--keygen', type=int, default=0,
                    help='key generation (key_area_key_application_XX), default 0')
    ap.add_argument('--name', default='SSX Tricky')
    ap.add_argument('--publisher', default='OpenTricky')
    ap.add_argument('--version', default='1.0')
    a = ap.parse_args()

    keys = read_keys(a.keys)
    need = ['header_key', 'key_area_key_application_%02x' % a.keygen]
    missing = [k for k in need if k not in keys]
    if missing:
        sys.exit('%s has no %s' % (a.keys, ', '.join(missing)))
    if len(keys['header_key']) != 32:
        sys.exit('header_key in %s is not 32 bytes' % a.keys)
    with open(a.nso, 'rb') as f:
        nso = f.read()
    with open(a.npdm, 'rb') as f:
        npdm = f.read()
    if struct.unpack_from('<Q', npdm, npdm.find(b'ACI0') + 0x10)[0] != TITLE_ID:
        sys.exit('%s is not the OpenTricky forwarder (title id)' % a.npdm)
    nsp = build(keys, a.keygen, nso, npdm, icon_jpeg(a.cover),
                a.name.encode()[:0x1FF], a.publisher.encode()[:0xFF], a.version.encode()[:0xF])
    with open(a.out, 'wb') as f:
        f.write(nsp)
    print('wrote %s (%d KiB, title id %016X)' % (a.out, len(nsp) // 1024, TITLE_ID))


if __name__ == '__main__':
    main()
