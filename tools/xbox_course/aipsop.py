"""AI paths and race lines (.aip/.sop), as SSX-Library's AIPSOPHandler and TrickyLevelInterface write them."""
import math
import struct


def _vectors(points):
    out = []
    for x, y, z in points[1:]:
        dist = math.hypot(x, y)
        if dist <= 0:
            continue
        out.append((x / dist, y / dist, z / dist, dist))
    return out


def _bounds(pos, points):
    cur = list(pos)
    lo, hi = list(cur), list(cur)
    for p in points:
        cur = [cur[k] + p[k] for k in range(3)]
        lo = [min(lo[k], cur[k]) for k in range(3)]
        hi = [max(hi[k], cur[k]) for k in range(3)]
    return lo, hi


def _events(path):
    out = b""
    for e in path.get("PathEvents") or []:
        out += struct.pack("<2i2f", e["EventType"], e["EventValue"], e["EventStart"], e["EventEnd"])
    return out


def build(js):
    """Slopesmith AIP.json / SOP.json -> file bytes."""
    body = bytearray()
    paths = js["AIPaths"]
    starts = js["StartPosList"]
    body += struct.pack("<2i", len(paths), len(starts)) + struct.pack(f"<{len(starts)}i", *starts)
    for p in paths:
        pts = [(0.0, 0.0, 0.0)] + [tuple(q) for q in p["PathPoints"]]
        vec = _vectors(pts)
        lo, hi = _bounds(p["PathPos"], pts)
        ev = p.get("PathEvents") or []
        body += struct.pack("<9i", 2, 100, 4, p["U3"], 101, 4, 1 if p["Respawnable"] else 0, len(vec), len(ev))
        body += struct.pack("<9f", *p["PathPos"], *lo, *hi)
        body += b"".join(struct.pack("<4f", *v) for v in vec) + _events(p)
    race_off = len(body)
    lines = bytearray()
    for p in js["RaceLines"]:
        pts = [(0.0, 0.0, 0.0)] + [tuple(q) for q in p["PathPoints"]]
        vec = _vectors(pts)
        lo, hi = _bounds(p["PathPos"], pts)
        ev = p.get("PathEvents") or []
        lines += struct.pack("<3if2i", 1, 0, 4, p["DistanceToFinish"], len(vec), len(ev))
        lines += struct.pack("<9f", *p["PathPos"], *lo, *hi)
        lines += b"".join(struct.pack("<4f", *v) for v in vec) + _events(p)
    tail = struct.pack("<2i", len(js["RaceLines"]), 0) + lines
    body += struct.pack("<2i", 1, len(tail)) + tail
    return b"\x0A\x0A\x0A\x0A" + struct.pack("<3i", 2, 0, race_off) + bytes(body)

