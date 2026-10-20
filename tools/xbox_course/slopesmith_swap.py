"""Put a Slopesmith export's terrain into an unpacked Xbox course (first test: TESTMTN into Garibaldi).

    swap.py <unpacked course> <slopesmith export> <out folder>

The course keeps its props, lights, splines and effects in the file; the new world grid lists only
the new terrain and the two StageArea markers, so the old props neither draw nor collide. The start
marker is moved onto the export's race line as Snowknife does on the PS2.
"""
import json
import math
import os
import shutil
import struct
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import aipsop  # noqa: E402
import dxt  # noqa: E402
import ltg  # noqa: E402
import patches as P  # noqa: E402

START_HASH, FINISH_HASH = 0x0091F640, 0x0FC656E0
FORMATION = np.array([-8.261667, 42.926667, 280.648346])   # stock six-rider formation, local to the start marker
# texture -> retail Garibaldi (texture, normal map) of the same kind of ground; the export's picture is
# encoded over that texture, and the normal map is kept
TEXTURES = {"snow.png": (12, 0), "oob.png": (28, 16), "offtrack.png": (20, 8), "powder.png": (19, 7),
            "ice.png": (93, 37), "rock.png": (30, 18)}


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def hash_index(x, h):
    hd = u32(x, 0x94)
    n, o = struct.unpack_from("<2I", x, hd + 12)
    for i in range(n):
        hs, ix = struct.unpack_from("<2I", x, hd + o + 8 * i)
        if hs == h:
            return ix
    raise KeyError(hex(h))


def instance_box(x, i):
    b = u32(x, 0x5C) + 144 * i
    return np.array(struct.unpack_from("<3f", x, b + 0x4C)), np.array(struct.unpack_from("<3f", x, b + 0x58))


def move_start(x, race):
    """Start marker onto the race line origin, facing its first horizontal step."""
    i = hash_index(x, START_HASH)
    b = u32(x, 0x5C) + 144 * i
    old = np.array(struct.unpack_from("<16f", x, b)).reshape(4, 4)
    step = next(p for p in race["PathPoints"] if p[0] * p[0] + p[1] * p[1] > 1e-4)
    yaw = math.atan2(step[1], step[0]) - math.pi
    c, s = math.cos(yaw), math.sin(yaw)
    new = np.array([[c, s, 0, 0], [-s, c, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1.0]])   # row-vector rotation about Z
    new[3, :3] = np.array(race["PathPos"]) - FORMATION @ new[:3, :3]
    lo, hi = instance_box(x, i)
    inv = np.linalg.inv(old)
    corners = np.array([[(lo, hi)[m & 1][0], (lo, hi)[m >> 1 & 1][1], (lo, hi)[m >> 2 & 1][2], 1] for m in range(8)])
    moved = corners @ inv @ new
    struct.pack_into("<16f", x, b, *new.reshape(16))
    struct.pack_into("<6f", x, b + 0x4C, *moved[:, :3].min(0), *moved[:, :3].max(0))
    return i, new[3, :3], math.degrees(yaw)


def lights(x):
    out = []
    for i in range(struct.unpack_from("<I", x, 0x24)[0]):
        b = u32(x, 0x6C) + 92 * i
        out.append((u32(x, b), np.array(struct.unpack_from("<3f", x, b + 52)), np.array(struct.unpack_from("<3f", x, b + 64))))
    return out


def xbox_lightmap(rgba):
    """A Slopesmith (PS2) lightmap page as an Xbox one. The PS2 page holds a GS blend residual: light =
    (0.5 - rgb) * alpha / 128 (Slopesmith decodeLightmapTexel, white diffuse); the Xbox page holds the light."""
    a = rgba.astype(float)
    light = np.clip((0.5 - a[:, :, :3] / 255) * (a[:, :, 3:4] / 128), 0, 1)
    out = np.empty(rgba.shape, np.uint8)
    out[:, :, :3] = np.round(light[:, :, ::-1] * 255)   # B, G, R
    out[:, :, 3] = 255
    return out.tobytes()


def regenerate(template, boxes, inst_boxes, light_list, size=10000.0, nw=4):
    """RegenerateCentreLTG: each patch and instance listed once, in the cell and node holding its box centre."""
    g = dict(head=template["head"], size=size, nw=nw, nsize=size / nw)
    lo, hi = np.zeros(3), np.zeros(3)
    for a, b in list(boxes) + list(inst_boxes.values()) + [(l[1], l[2]) for l in light_list]:
        lo, hi = np.minimum(lo, a), np.maximum(hi, b)
    g["wb"] = [*lo, *hi, *template["wb"][6:9]]

    def cell_end(v):
        return math.trunc(v / size) + (1 if math.fmod(v, size) else 0)
    x0 = math.trunc(lo[0] / size) - (1 if math.fmod(lo[0], size) else 0)
    y0 = math.trunc(lo[1] / size) - (1 if math.fmod(lo[1], size) else 0)
    x1, y1 = cell_end(hi[0]), cell_end(hi[1])
    nx, ny = x1 - x0, y1 - y0
    g["nx"], g["ny"] = nx, ny
    centre = lambda a, b: ((a[0] + b[0]) / 2, (a[1] + b[1]) / 2)
    cell_of = {}
    cells, used = [], 0
    for cy in range(ny):
        for cx in range(nx):
            bx0, by0 = (x0 + cx) * size, (y0 + cy) * size
            inside = lambda c, ax, ay, w: ax <= c[0] < ax + w and ay <= c[1] < ay + w
            pl = [i for i, (a, b) in enumerate(boxes) if i not in cell_of and inside(centre(a, b), bx0, by0, size)]
            il = [i for i, (a, b) in inst_boxes.items() if inside(centre(a, b), bx0, by0, size)]
            if not pl and not il:
                cells.append(None)
                continue
            for i in pl:
                cell_of[i] = cy * nx + cx
            used += 1
            crossing = [i for i, (t, a, b) in enumerate(light_list)
                        if t == 3 or (a[0] <= bx0 + size and b[0] >= bx0 and a[1] <= by0 + size and b[1] >= by0)]
            nodes = []
            for ny_ in range(nw):
                for nx_ in range(nw):
                    ax, ay, w = bx0 + nx_ * g["nsize"], by0 + ny_ * g["nsize"], g["nsize"]
                    np_ = [i for i in pl if inside(centre(*boxes[i]), ax, ay, w)]
                    ni = [i for i in il if inside(centre(*inst_boxes[i]), ax, ay, w)]
                    nc = [i for i in crossing if light_list[i][0] == 3 or (
                        light_list[i][1][0] <= ax + w and light_list[i][2][0] >= ax and
                        light_list[i][1][1] <= ay + w and light_list[i][2][1] >= ay)]
                    wlo, whi = np.array([ax, ay, 0.0]), np.array([ax + w, ay + w, 0.0])
                    items = [boxes[i] for i in np_] + [inst_boxes[i] for i in ni]
                    if items:
                        wlo = np.min([a for a, _ in items], 0)
                        whi = np.max([b for _, b in items], 0)
                    wlo, whi = wlo - 50, whi + 50
                    nodes.append(dict(wb=[*wlo, *whi, *((wlo + whi) / 2)],
                                      counts=[len(np_), len(ni), len(ni), 0, 0, len(nc), 0, 0],
                                      lists=[np_, ni, [], [], nc, []], modified=bool(items)))
            mod = [n for n in nodes if n["modified"]]
            clo = np.min([n["wb"][0:3] for n in mod], 0)
            chi = np.max([n["wb"][3:6] for n in mod], 0)
            counts = [len(pl), len(il), 0, 0, sum(len(n["lists"][4]) for n in nodes), 0]
            cells.append(dict(wb=[*clo, *chi, *((clo + chi) / 2)], counts=counts, unk1=0, total=sum(counts), nodes=nodes))
    g["cells"], g["used"], g["empty"] = cells, used, nx * ny - used
    return g, cell_of


def main(src, export, out):
    if os.path.exists(out):
        shutil.rmtree(out)
    shutil.copytree(src, out)
    name = next(f[:-4] for f in os.listdir(out) if f.endswith(".xbd") and "_sky" not in f)
    x = bytearray(open(os.path.join(out, name + ".xbd"), "rb").read())
    pal = P.palette_of(x, u32(x, 0x98))
    pj = json.load(open(os.path.join(export, "Patches.json")))["Patches"]
    old_n = u32(x, 0x08)
    if len(pj) > old_n:
        raise SystemExit(f"{len(pj)} patches do not fit the slot's {old_n}")
    raws = [np.array(p["Points"], float) for p in pj]
    boxes = [(r.min(0), r.max(0)) for r in raws]

    aip = json.load(open(os.path.join(export, "AIP.json")))
    sop = json.load(open(os.path.join(export, "SOP.json")))
    si, pos, yaw = move_start(x, sop["RaceLines"][0])
    print(f"start marker {si} -> ({pos[0]:.1f}, {pos[1]:.1f}, {pos[2]:.1f}), yaw {yaw:.1f}")
    keep = {i: instance_box(x, i) for i in (si, hash_index(x, FINISH_HASH))}

    tmpl = ltg.read_ltg(open(os.path.join(out, name + ".ltg"), "rb").read())
    g, cell_of = regenerate(tmpl, boxes, keep, lights(x))
    open(os.path.join(out, name + ".ltg"), "wb").write(ltg.write_ltg(g))
    print(f"grid {g['nx']}x{g['ny']}, {g['used']} cells used; {len(cell_of)} of {len(pj)} patches listed")

    po, so = u32(x, 0x50), u32(x, 0x54)
    for i, p in enumerate(pj):
        t0, nm = TEXTURES[p["TexturePath"]]
        rec, srcpts = P.build(raws[i], p["LightMapPoint"], p["UVPoints"], p["SurfaceType"], cell_of[i],
                              t0, nm, p["LightmapID"], p["TrickOnlyPatch"], pal)
        x[po + P.PATCH * i:po + P.PATCH * (i + 1)] = rec
        x[so + P.SRC * i:so + P.SRC * (i + 1)] = srcpts
    struct.pack_into("<I", x, 0x08, len(pj))
    open(os.path.join(out, name + ".xbd"), "wb").write(x)

    open(os.path.join(out, name + ".aip"), "wb").write(aipsop.build(aip))
    open(os.path.join(out, name + ".sop"), "wb").write(aipsop.build(sop))

    # lightmap pages: the export's 128x128 pages over the first ones of the slot, linear A8R8G8B8
    lpath = os.path.join(out, name + "_L.xsh")
    L = bytearray(open(lpath, "rb").read())
    pages = sorted(f for f in os.listdir(os.path.join(export, "Lightmaps")) if f.endswith(".png"))
    for k, f in enumerate(pages):
        im = Image.open(os.path.join(export, "Lightmaps", f)).convert("RGBA")
        o = u32(L, 20 + 8 * k)
        if (L[o], *struct.unpack_from("<2H", L, o + 4)) != (0x7D, *im.size):
            raise SystemExit(f"lightmap page {k} is not a {im.size} A8R8G8B8 page")
        L[o + 16:o + 16 + im.size[0] * im.size[1] * 4] = xbox_lightmap(np.asarray(im))
    open(lpath, "wb").write(L)
    print(f"{len(pj)} patches, {len(pages)} lightmap pages written")

    # the export's textures over the slot textures they replace: same size, format and mip count,
    # so no offset in the bank moves. The slot's props used them too, but those are delisted.
    tpath = os.path.join(out, name + ".xsh")
    T = bytearray(open(tpath, "rb").read())
    used = sorted({p["TexturePath"] for p in pj})
    for f in used:
        o = u32(T, 20 + 8 * TEXTURES[f][0])
        kind, (w, h) = T[o], struct.unpack_from("<2H", T, o + 4)
        size = int.from_bytes(T[o + 1:o + 4], "little") - 16
        if kind != 0x60:
            raise SystemExit(f"texture {TEXTURES[f][0]} is not DXT1")
        im = Image.open(os.path.join(export, "Textures", f)).convert("RGB")
        data, mw = b"", w
        while len(data) < size and mw >= 4:
            data += dxt.encode(np.asarray(im.resize((mw, mw * h // w), Image.LANCZOS)))
            mw //= 2
        if len(data) != size:
            raise SystemExit(f"texture {TEXTURES[f][0]}: {len(data)} bytes encoded for {size}")
        T[o + 16:o + 16 + size] = data
    open(tpath, "wb").write(T)
    print(f"{len(used)} textures encoded")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
