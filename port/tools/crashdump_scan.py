#!/usr/bin/env python3
"""Count the player's paths and name left in a crash report (crash.dmp, hang.dmp).

    crashdump_scan.py REPORT_DIR_OR_DMP [--folder DIR ...] [--name NAME ...]
                      [--no-defaults] [--texts] [--ignore TEXT] [-v]
    crashdump_scan.py --selftest

Looks for every folder in every form Windows can store it in:
    E:\\Play\\OpenTricky            (with the drive letter)
    \\Play\\OpenTricky              (without: what any other form contains)
    \\Device\\HarddiskVolume3\\Play\\OpenTricky   (NT form, from QueryDosDeviceW)
    \\??\\E:\\Play\\OpenTricky  and  \\\\?\\E:\\Play\\OpenTricky
and for every name, each one as UTF-16LE, UTF-8 and the ANSI code page,
case-insensitive (Unicode simple case: a pattern letter matches its lower and
upper case), at any byte offset. Also the folder's own name (5 letters or
more), which a partly blanked path still shows. The ANSI form is what the
...A functions of Windows give (best-fit letters, '?').

Defaults (unless --no-defaults): the install root of the report (the folder
holding CrashReports\\), the folder of the dump's game executable when it is in
bin\\, %USERPROFILE% and %USERNAME%. Each hit is placed in its minidump stream
(module list, memory list, ...), to tell where a leak comes from.

--texts also scans report.txt, log.txt and settings.txt of the folder, with
their own verdict (TEXTS SCAN): the texts keep file names, never a folder's.

Exit status: 0 when the dump (and with --texts, the texts) hold none of them,
1 otherwise, 2 on error.
"""
import argparse, ctypes, os, re, struct, sys

verbose = False                  # -v: every string searched, not only the ones found

STREAM_NAMES = {
    3: "ThreadList", 4: "ModuleList", 5: "MemoryList", 6: "Exception",
    7: "SystemInfo", 9: "Memory64List", 12: "HandleData", 14: "UnloadedModuleList",
    15: "MiscInfo", 16: "MemoryInfoList", 17: "ThreadInfoList", 21: "SystemMemoryInfo",
    22: "ProcessVmCounters", 24: "ThreadNames",
}


def ansi_codec():
    try:
        return "cp%d" % ctypes.windll.kernel32.GetACP()
    except Exception:
        return "cp1252"


def nt_device(drive):
    """'D:' -> '\\Device\\HarddiskVolume3' (None off Windows or on failure)."""
    try:
        buf = ctypes.create_unicode_buffer(1024)
        if ctypes.windll.kernel32.QueryDosDeviceW(drive, buf, 1024):
            return buf.value
    except Exception:
        pass
    return None


def short_path(p):
    """The 8.3 form of p ("C:\\PROGRA~1\\..."), or None."""
    try:
        buf = ctypes.create_unicode_buffer(1024)
        if ctypes.windll.kernel32.GetShortPathNameW(p, buf, 1024):
            return buf.value
    except Exception:
        pass
    return None


def folder_forms(path):
    """The forms of one folder (label, string) a dump could hold."""
    p = os.path.normpath(os.path.abspath(path)).rstrip("\\")
    forms = [("full", p)]
    if len(p) >= 2 and p[1] == ":":
        tail = p[2:]
        forms.append(("no-drive", tail))
        dev = nt_device(p[:2].upper())
        if dev:
            forms.append(("device", dev + tail))
        forms.append(("\\??\\", "\\??\\" + p))
        forms.append(("\\\\?\\", "\\\\?\\" + p))
    elif p.startswith("\\\\"):
        forms.append(("no-server-prefix", p[1:]))
    short = short_path(p)
    if short and short.lower() != p.lower():
        forms.append(("8.3", short[2:] if short[1:2] == ":" else short))
    leaf = os.path.basename(p)
    if len(leaf) >= 5:
        # The folder's own name: still there when only a part of the path
        # was blanked (a parent folder or the user name inside it).
        forms.append(("leaf", leaf))
    return forms


def ci_regex(s, codec):
    """A bytes regex matching s in codec, each letter in either case."""
    parts = []
    for ch in s:
        alts = {ch, ch.lower(), ch.upper()}
        enc = set()
        for a in alts:
            if len(a) != 1:
                continue
            if codec == "ansi":
                e = acp_bytes(a)
                if e:
                    enc.add(e)
                continue
            try:
                enc.add(a.encode(codec))
            except UnicodeEncodeError:
                pass
        if not enc:
            return None                # not representable in this code page
        parts.append(b"(?:" + b"|".join(re.escape(e) for e in sorted(enc)) + b")")
    return re.compile(b"".join(parts))


def acp_bytes(ch):
    """ch as the ...A functions of Windows give it: the ANSI code page with
    its best-fit letters and '?' (WideCharToMultiByte(CP_ACP, 0))."""
    try:
        buf = ctypes.create_string_buffer(16)
        n = ctypes.windll.kernel32.WideCharToMultiByte(0, 0, ch, len(ch), buf, 16, None, None)
        return buf.raw[:n] if n > 0 else None
    except Exception:
        try:
            return ch.encode(ansi_codec())
        except UnicodeEncodeError:
            return None


def encodings():
    out = [("utf16", "utf-16-le"), ("utf8", "utf-8")]
    if ansi_codec().lower() != "cp65001":
        out.append(("ansi", "ansi"))
    return out


def streams(data):
    """[(start, end, name)] of a minidump's streams and memory ranges."""
    res = []
    try:
        return _streams(data, res)
    except struct.error:
        return res      # not a minidump, or a cut one


def _streams(data, res):
    if data[:4] != b"MDMP":
        return res
    _, _, n, rva = struct.unpack_from("<IIII", data, 0)
    for k in range(n):
        typ, size, srva = struct.unpack_from("<III", data, rva + 12 * k)
        name = STREAM_NAMES.get(typ, "stream%d" % typ)
        res.append((srva, srva + size, name))
        if typ == 5:                     # MemoryList: each range's bytes
            cnt = struct.unpack_from("<I", data, srva)[0]
            for m in range(cnt):
                base, dsize, drva = struct.unpack_from("<QII", data, srva + 4 + 16 * m)
                res.append((drva, drva + dsize, "Memory@%#x" % base))
        if typ == 3:                     # ThreadList: each stack
            cnt = struct.unpack_from("<I", data, srva)[0]
            for m in range(cnt):
                off = srva + 4 + 48 * m
                tid = struct.unpack_from("<I", data, off)[0]
                base, dsize, drva = struct.unpack_from("<QII", data, off + 24)
                res.append((drva, drva + dsize, "Stack(tid %d)" % tid))
    return res


def where(sts, off):
    best = None
    for s, e, name in sts:
        if s <= off < e and (best is None or e - s < best[1] - best[0]):
            best = (s, e, name)
    return best[2] if best else "header/other"


def scan(data, patterns, is_dump=True):
    """patterns: [(label, string)] -> [(label, form, enc, count, places)]."""
    sts = streams(data) if is_dump else []
    rows = []
    for label, s in patterns:
        for ename, codec in encodings():
            rx = ci_regex(s, codec)
            if rx is None:
                rows.append((label, s, ename, None, []))
                continue
            hits = [m.start() for m in rx.finditer(data)]
            places = sorted({where(sts, h) for h in hits}) if is_dump else []
            rows.append((label, s, ename, len(hits), places))
    return rows


def default_patterns(dmp):
    pats = []
    rep = os.path.dirname(os.path.abspath(dmp))
    cr = os.path.dirname(rep)
    if os.path.basename(cr).lower() == "crashreports":
        root = os.path.dirname(cr)
        for lab, f in folder_forms(root):
            pats.append(("game " + lab, f))
    prof = os.environ.get("USERPROFILE")
    if prof:
        for lab, f in folder_forms(prof):
            pats.append(("profile " + lab, f))
    user = os.environ.get("USERNAME")
    if user:
        pats.append(("user name", user))
    return pats


def print_rows(title, rows):
    print("== " + title)
    total = 0
    for label, s, ename, cnt, places in rows:
        if cnt is None:
            continue                       # not representable in that code page
        total += cnt
        if cnt or verbose:
            extra = ("  in " + ", ".join(places)) if places else ""
            print("  %-20s %-5s %4d  %s%s" % (label, ename, cnt, s, extra))
    print("  total: %d (%d strings x encodings searched)" % (total, len(rows)))
    return total


def selftest():
    folder = "D:\\Jeux\\\u00c9lodie \u00dcn\u00efc\u00f8d\u00e9\\SSX"
    name = "\u00c9lodie"
    blob = bytearray(b"MDMPjunk")
    blob += ("\\Device\\HarddiskVolume3" + folder[2:]).upper().encode("utf-16-le")
    blob += b"\x01" + folder.encode("utf-8") + b"\x00"
    blob += name.lower().encode("utf-16-le")[1:]           # misaligned on purpose
    blob += b"\x00" + name.lower().encode("utf-16-le")
    pats = [("game " + l, f) for l, f in folder_forms(folder)] + [("user name", name)]
    rows = {(r[0], r[2]): r[3] for r in scan(bytes(blob), pats, is_dump=False)}
    ok = (rows[("game no-drive", "utf16")] == 1 and rows[("game no-drive", "utf8")] == 1
          and rows[("game full", "utf8")] == 1 and rows[("game full", "utf16")] == 0
          and rows[("user name", "utf16")] == 2 and rows[("user name", "utf8")] == 1)
    dev = nt_device("D:")
    if dev == "\\Device\\HarddiskVolume3":
        ok = ok and rows[("game device", "utf16")] == 1
    print("SELFTEST " + ("PASS" if ok else "FAIL"), rows if not ok else "")
    return 0 if ok else 1


def drop(data, ignore):
    """data without the --ignore strings (UTF-8 and UTF-16), same length."""
    for t in ignore:
        for e in (t.encode("utf-8"), t.encode("utf-16-le")):
            data = data.replace(e, b"\x00" * len(e))
    return data


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")   # paths with any letters
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("target", nargs="?")
    ap.add_argument("--folder", action="append", default=[])
    ap.add_argument("--name", action="append", default=[])
    ap.add_argument("--no-defaults", action="store_true")
    ap.add_argument("--texts", action="store_true")
    ap.add_argument("--ignore", action="append", default=[],
                    help="a public string left out before counting (a URL holding the user name)")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    global verbose
    verbose = a.verbose
    if a.selftest:
        return selftest()
    if not a.target:
        ap.error("a report folder or a .dmp is needed")
    dumps = []
    if os.path.isdir(a.target):
        for f in ("crash.dmp", "hang.dmp"):
            if os.path.isfile(os.path.join(a.target, f)):
                dumps.append(os.path.join(a.target, f))
    else:
        dumps.append(a.target)
    if not dumps:
        print("no dump in " + a.target)
        return 2
    total = texts = 0
    for d in dumps:
        pats = [] if a.no_defaults else default_patterns(d)
        for f in a.folder:
            pats += [("folder " + l, s) for l, s in folder_forms(f)]
        pats += [("name", n) for n in a.name]
        with open(d, "rb") as fh:
            data = drop(fh.read(), a.ignore)
        total += print_rows("%s (%d bytes)" % (d, len(data)), scan(data, pats))
        if a.texts:
            rep = os.path.dirname(os.path.abspath(d))
            for t in ("report.txt", "log.txt", "settings.txt"):
                p = os.path.join(rep, t)
                if os.path.isfile(p):
                    with open(p, "rb") as fh:
                        texts += print_rows(p + " (text)", scan(drop(fh.read(), a.ignore), pats, is_dump=False))
    print("CRASHDUMP SCAN " + ("PASS (nothing left)" if total == 0 else "FAIL (%d left)" % total))
    if a.texts:
        print("TEXTS SCAN " + ("PASS (nothing left)" if texts == 0 else "FAIL (%d left)" % texts))
    return 0 if total == 0 and texts == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
