"""Terrain patches of an Xbox .xbd: build the 0x2D0-byte record and the 16 source points."""
import struct
import numpy as np

PATCH = 0x2D0
SRC = 20 * 16
M = np.array([[-1, 3, -3, 1], [3, -6, 3, 0], [-3, 3, 0, 0], [1, 0, 0, 0]], float)


def coefficients(raw):
    """raw: (16, 3) Bezier points, k = 4i+j -> C[r][k] (4, 4, 3)."""
    R = raw.reshape(4, 4, 3)
    return np.einsum("ri,kj,ijc->rkc", M, M, R)


def _basis(t):
    return np.array([t ** 3, t ** 2, t, 1.0]), np.array([3 * t ** 2, 2 * t, 1.0, 0.0])


def frames(C):
    """Normal and tangent at the 8x8 vertices, idx = 8j+i at a = j/7, b = i/7."""
    nrm, tan = np.zeros((64, 3)), np.zeros((64, 3))
    for j in range(8):
        Ba, dBa = _basis(j / 7)
        for i in range(8):
            Bb, dBb = _basis(i / 7)
            dPa = np.einsum("r,k,rkc->c", Bb, dBa, C)
            dPb = np.einsum("r,k,rkc->c", dBb, Ba, C)
            n = -np.cross(dPa, dPb)
            nrm[8 * j + i] = n / (np.linalg.norm(n) or 1)
            tan[8 * j + i] = dPb / (np.linalg.norm(dPb) or 1)
    return nrm, tan


def nearest(palette, vecs):
    return np.argmax(vecs @ palette.T, axis=1).astype(np.uint16)


def build(raw, lightmap_rect, uvs, surface, cell, t0, nm, lm, trick, palette):
    """One patch record and its source points. raw (16,3); uvs 4 (u, v) corners."""
    raw = np.asarray(raw, float)
    C = coefficients(raw)
    rec = bytearray(PATCH)
    struct.pack_into("<4f", rec, 0x00, *lightmap_rect)
    for k, (u, v) in enumerate(uvs):
        struct.pack_into("<4f", rec, 0x10 + 16 * k, u, v, 1.0, 1.0)
    for r in range(4):
        for k in range(4):
            struct.pack_into("<4f", rec, 0x50 + 0x40 * r + 0x10 * k, *C[r][k], 1.0)
    struct.pack_into("<6f", rec, 0x150, *raw.min(0), *raw.max(0))
    struct.pack_into("<6I", rec, 0x168, surface, 4, 4, 4, 4, cell)
    for k, idx in enumerate((0, 12, 3, 15)):
        struct.pack_into("<4f", rec, 0x180 + 16 * k, *raw[idx], 1.0)
    struct.pack_into("<4h", rec, 0x1C0, t0, nm, lm, -1)
    nrm, tan = frames(C)
    struct.pack_into("<64H", rec, 0x1C8, *nearest(palette, nrm))
    struct.pack_into("<64H", rec, 0x248, *nearest(palette, tan))
    struct.pack_into("<HHI", rec, 0x2C8, 337, 0x8000 if trick else 0, 156)
    # source points: uv bilinear over the corners, matching retail (i/3, j/3 - 1) on 0..1 patches
    A, B, Cc, D = uvs[2], uvs[3], uvs[0], uvs[1]
    src = bytearray()
    for k in range(16):
        s, t = (k // 4) / 3, (k % 4) / 3
        u = (1 - s) * (1 - t) * A[0] + s * (1 - t) * B[0] + (1 - s) * t * Cc[0] + s * t * D[0]
        v = (1 - s) * (1 - t) * A[1] + s * (1 - t) * B[1] + (1 - s) * t * Cc[1] + s * t * D[1]
        src += struct.pack("<5f", *raw[k], u, v)
    return bytes(rec), bytes(src)


def palette_of(xbd, off98, count=4098):
    return np.frombuffer(xbd, "<f4", count * 3, off98).reshape(count, 3).astype(float)
