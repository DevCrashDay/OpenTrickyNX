"""Models of an Xbox .xbd (see docs/xbox-level-format.md, Models): read their layout, and put new geometry
into an existing model's room in place, so that no offset in the file moves."""
import struct

import numpy as np


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def read_models(x):
    """[{off, objects: [(obj off, high, med, low header offs)], vrange: (first, last) pool index}]"""
    n = u32(x, 0x34)
    ptrs = u32(x, 0x7C)
    out = []
    for m in range(n):
        mo = u32(x, 0x80) + u32(x, ptrs + 4 * m)      # pointers count from the models section
        objs, idx = [], []
        for k in range(u32(x, mo + 4)):
            ob = mo + u32(x, mo + 8) + 0x18 * k
            hs = [ob + v if v else 0 for v in struct.unpack_from("<3I", x, ob + 4)]
            objs.append((ob, *hs))
            for h in set(v for v in hs if v):
                for mesh in meshes(x, h):
                    idx += strip(x, mesh)
        out.append(dict(off=mo, objects=objs, vrange=(min(idx), max(idx)) if idx else None))
    return out


def meshes(x, h):
    tab = h + u32(x, h + 0x28)
    return [tab + 4 * i + u32(x, tab + 4 * i) for i in range(u32(x, h + 0x20))]


def strip(x, mesh):
    p = mesh + u32(x, mesh + 8)
    out = []
    for _ in range(struct.unpack_from("<H", x, p)[0]):
        n = struct.unpack_from("<H", x, p + 2)[0]
        out += list(struct.unpack_from(f"<{n}H", x, p + 4))
        p += 2 + 2 * n
    return out


def strip_room(x, mesh):
    """Indices the first strip of a mesh can hold."""
    return struct.unpack_from("<H", x, mesh + u32(x, mesh + 8) + 2)[0]


def triangles_to_strip(tris):
    """Triangles as one strip joined by repeated indices; each triangle starts on an even position so its
    winding is kept."""
    out = []
    for a, b, c in tris:
        if out:
            out += [out[-1], a]
            if len(out) % 2:
                out.append(a)
        out += [a, b, c]
    return out


def put_mesh(x, model, verts, tris):
    """Replace a model's geometry: verts (n, 8) position, normal, uv in model space; tris index verts.
    The model must have one object whose high-detail header holds one mesh with room for the strip, and a
    vertex run of at least n. Medium and low detail are pointed at the high header. Returns False if it
    does not fit."""
    if len(model["objects"]) != 1 or not model["vrange"]:
        return False
    ob, high = model["objects"][0][:2]
    ms = meshes(x, high)
    first, last = model["vrange"]
    seq = [first + i for i in triangles_to_strip(tris)]
    if len(ms) != 1 or len(verts) > last - first + 1 or len(seq) > strip_room(x, ms[0]):
        return False
    pool = u32(x, 0x58) + 12
    filler = verts[0]
    for i in range(last - first + 1):
        struct.pack_into("<8f", x, pool + 32 * (first + i), *(verts[i] if i < len(verts) else filler))
    p = ms[0] + u32(x, ms[0] + 8)
    room = strip_room(x, ms[0])
    seq += [seq[-1]] * (room - len(seq))        # pad with repeats: degenerate, nothing drawn
    struct.pack_into(f"<HH{room}H", x, p, 1, room, *seq)
    lo, hi = verts[:, :3].min(0), verts[:, :3].max(0)
    struct.pack_into("<6f", x, high + 4, *lo, *hi)
    struct.pack_into("<3f", x, model["off"] + 0x1C, *hi)
    struct.pack_into("<2I", x, ob + 8, high - ob, high - ob)
    return True


def load_obj(path):
    """Positions, uvs and faces (vertex indices) of a Slopesmith prop mesh."""
    v, vt, faces = [], [], []
    for line in open(path):
        w = line.split()
        if not w:
            continue
        if w[0] == "v":
            v.append([float(a) for a in w[1:4]])
        elif w[0] == "vt":
            vt.append([float(a) for a in w[1:3]])
        elif w[0] == "f":
            faces.append([int(a.split("/")[0]) - 1 for a in w[1:]])
    return np.array(v), np.array(vt), faces


def flat_mesh(v, vt, faces, double_sided=True):
    """Per-face vertices with flat normals; both sides when asked (Slopesmith props are often open)."""
    verts, tris = [], []
    for f in faces:
        p = v[f]
        n = np.cross(p[1] - p[0], p[2] - p[0])
        n = n / (np.linalg.norm(n) or 1)
        for side in ((1, False), (-1, True)) if double_sided else ((1, False),):
            base = len(verts)
            for k, i in enumerate(f):
                uv = vt[i] if i < len(vt) else (0.5, 0.5)
                verts.append([*v[i], *(side[0] * n), uv[0], uv[1]])
            for k in range(1, len(f) - 1):
                t = (base, base + k, base + k + 1)
                tris.append((t[0], t[2], t[1]) if side[1] else t)
    return np.array(verts), tris
