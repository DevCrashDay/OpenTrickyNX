"""World grid (.ltg): read, write, regenerate. Layout as SSX-Library's LTGHandler."""
import struct

KINDS = ("patches", "instances", "splines", "lights", "crossing", "particles")


def read_ltg(d):
    g = {"head": d[:4]}
    g["wb"] = list(struct.unpack_from("<9f", d, 4))
    (g["size"], g["nx"], g["ny"], total, g["used"], g["empty"], g["nsize"], g["nw"], g["ncount"],
     plo, blo) = struct.unpack_from("<f5if2i2i", d, 40)
    offs = struct.unpack_from(f"<{total}i", d, plo)
    cells = []
    for o in offs:
        if not o:
            cells.append(None)
            continue
        c = {"wb": list(struct.unpack_from("<9f", d, o))}
        c["counts"] = list(struct.unpack_from("<6h", d, o + 36))
        c["unk1"], c["total"], hsz = struct.unpack_from("<3i", d, o + 48)
        c["listoffs"] = struct.unpack_from("<6i", d, o + 60)
        nodes = []
        for k in range(g["nw"] * g["nw"]):
            p = o + 84 + 76 * k
            n = {"wb": list(struct.unpack_from("<9f", d, p))}
            n["counts"] = list(struct.unpack_from("<8h", d, p + 36))
            no = struct.unpack_from("<6i", d, p + 52)
            # patch, inst+gem, spline, light, crossing, particle lists
            cnt = [n["counts"][0], n["counts"][2], n["counts"][3], n["counts"][4], n["counts"][5], n["counts"][6]]
            n["lists"] = [list(struct.unpack_from(f"<{c_}i", d, o + off)) if c_ else [] for c_, off in zip(cnt, no)]
            n["offs"] = no
            nodes.append(n)
        c["nodes"] = nodes
        cells.append(c)
    g["cells"] = cells
    return g


def write_ltg(g):
    nx, ny, nw = g["nx"], g["ny"], g["nw"]
    total = nx * ny
    out = bytearray(g["head"])
    out += struct.pack("<9f", *g["wb"])
    out += struct.pack("<f5if2i", g["size"], nx, ny, total, g["used"], g["empty"], g["nsize"], nw, nw * nw)
    out += struct.pack("<2i", len(out) + 8, len(out) + 8 + total * 4)
    ptr_at = len(out)
    out += b"\0" * (total * 4)
    offs = []
    for c in g["cells"]:
        if c is None:
            offs.append(0)
            continue
        base = len(out)
        offs.append(base)
        body = bytearray()
        listoffs = []
        pos = 84 + 76 * nw * nw
        node_offs = [[0] * 6 for _ in c["nodes"]]
        for kind in range(6):
            if not c["counts"][kind]:
                listoffs.append(0)
                continue
            listoffs.append(pos + len(body))
            for k, n in enumerate(c["nodes"]):
                if n["lists"][kind]:
                    node_offs[k][kind] = pos + len(body)
                    body += struct.pack(f"<{len(n['lists'][kind])}i", *n["lists"][kind])
        hdr = struct.pack("<9f", *c["wb"]) + struct.pack("<6h", *c["counts"])
        hdr += struct.pack("<3i", c["unk1"], c["total"], 84) + struct.pack("<6i", *listoffs)
        for k, n in enumerate(c["nodes"]):
            hdr += struct.pack("<9f", *n["wb"]) + struct.pack("<8h", *n["counts"]) + struct.pack("<6i", *node_offs[k])
        out += hdr + body
    struct.pack_into(f"<{total}i", out, ptr_at, *offs)
    return bytes(out)
