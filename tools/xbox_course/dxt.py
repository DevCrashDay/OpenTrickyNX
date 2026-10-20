"""DXT1 (BC1) encode and decode for the Xbox texture banks; opaque blocks only."""
import numpy as np


def _565(c):
    c = np.clip(np.round(c), 0, 255).astype(np.int32)
    return (c[..., 0] >> 3) << 11 | (c[..., 1] >> 2) << 5 | (c[..., 2] >> 3)


def _rgb(v):
    v = v.astype(np.int32)
    return np.stack([(v >> 11) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31], -1).astype(float)


def encode(rgb):
    """rgb: (h, w, 3) uint8, h and w multiples of 4 -> DXT1 bytes, blocks row by row."""
    h, w, _ = rgb.shape
    blk = rgb.reshape(h // 4, 4, w // 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 3).astype(float)
    mean = blk.mean(1, keepdims=True)
    cov = np.einsum("bki,bkj->bij", blk - mean, blk - mean)
    axis = np.linalg.eigh(cov)[1][:, :, -1]                  # principal colour axis per block
    t = np.einsum("bki,bi->bk", blk - mean, axis)
    hi = mean[:, 0] + axis * t.max(1, keepdims=True)
    lo = mean[:, 0] + axis * t.min(1, keepdims=True)
    a, b = _565(hi), _565(lo)
    swap = a < b
    a, b = np.where(swap, b, a), np.where(swap, a, b)
    same = a == b                                            # flat block: any index works
    b = np.where(same & (b > 0), b - 1, b)
    a = np.where(same & (b == a), a + 1, a)
    ca, cb = _rgb(a), _rgb(b)
    pal = np.stack([ca, cb, (2 * ca + cb) / 3, (ca + 2 * cb) / 3], 1)   # (n, 4, 3)
    idx = np.argmin(((blk[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(-1), -1).astype(np.uint32)
    bits = (idx << (2 * np.arange(16, dtype=np.uint32))).sum(1).astype(np.uint32)
    out = np.zeros((len(blk), 2), np.uint32)
    out[:, 0] = a.astype(np.uint32) | b.astype(np.uint32) << 16
    out[:, 1] = bits
    return out.astype("<u4").tobytes()


def decode(data, w, h):
    blocks = np.frombuffer(data, "<u4", (w // 4) * (h // 4) * 2).reshape(-1, 2)
    a, b = blocks[:, 0] & 0xFFFF, blocks[:, 0] >> 16
    ca, cb = _rgb(a), _rgb(b)
    opaque = (a > b)[:, None]
    c2 = np.where(opaque, (2 * ca + cb) / 3, (ca + cb) / 2)
    c3 = np.where(opaque, (ca + 2 * cb) / 3, 0)
    pal = np.stack([ca, cb, c2, c3], 1)
    idx = (blocks[:, 1:2] >> (2 * np.arange(16, dtype=np.uint32))) & 3
    px = np.take_along_axis(pal, idx[:, :, None].astype(np.int64), 1)
    return px.reshape(h // 4, w // 4, 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(h, w, 3).astype(np.uint8)
