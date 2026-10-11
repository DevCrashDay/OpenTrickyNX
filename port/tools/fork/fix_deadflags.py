#!/usr/bin/env python3
"""Fork pass: revive three `if (_flags ...)` branches that fixsplitflags.py misses.

When the lifter splits a machine function between a flag-setting instruction
and the `jcc` that reads it, the consumer chunk branches on a local
`_flags = 0` that nothing sets: the branch is never taken. fixsplitflags.py
repairs the sites whose producer leaves a `(void)0; /* test|cmp a, b - flags
set for next jcc */` comment. Two other producer shapes are left alone:

  form 1  the comparison is fused into a conditional tail call on one line
              if (TEST_NZ(ebp, ebp)) { g_seh_ebp = ebp; sub_X(); return; }
              if (CMP_GE(edi & edi, 0)) { g_seh_ebp = ebp; sub_X(); return; }
  form 2  the flags come from a `dec` / `inc`, written without a comment
              edi--;
              g_seh_ebp = ebp; sub_X(); return;   /* implicit fall-through */

In both forms the consumer's `jcc` is its first instruction and nothing runs
between the producer and it, so the condition can be rebuilt in the consumer
from the same registers. This pass recognises both forms (32-, 16- and 8-bit
operands for form 1; form 2 only for je/jne, since dec/inc leave CF alone),
checks that *every* caller of the consumer has the same shape, and rewrites
only the sites listed in SITES below, each behind one bit of g_fix_deadflags
(XBOX_FIX_DEADFLAGS*, port/src/main.c; bit clear = branch never taken, as
before):

  0x1  sub_00019468  jle  producer 0x1942D `test ebp,ebp ; jne`   -> ebp <= 0
       negative length taken as positive: `rep movsd` with ecx ~ 2^32
  0x2  sub_000BE680  jne  producer 0xBE67F `dec edi` (loop back edge) -> edi != 0
       track-zone loop of the respawn point (0xBE5F0) only reads the first zone
  0x4  sub_001281DB  je   producer 0x1281C8 `test edi,edi ; jge`  -> edi == 0
       RLE decoder of the collision cache writes one byte past each block

Other consumers that match form 1 or 2 are only reported ("eligible, not
listed"). Each listed site also pins the condition it expects, so a change in
the generated code stops the pass instead of producing a different fix.

    fix_deadflags.py GEN_DIR [--dry-run]
Idempotent.
"""
import glob, io, os, re, sys

TAG = "/* fork_deadflags */"
DECL = "extern int g_fix_deadflags; " + TAG

# consumer -> (switch bit, expected condition, note)
SITES = {
    "sub_00019468": (0x1, "(int32_t)(ebp) <= 0", "flags from `test ebp, ebp` in sub_0001942D"),
    "sub_000BE680": (0x2, "(uint32_t)(edi) != 0", "flags from `dec edi` in sub_000BE610"),
    "sub_001281DB": (0x4, "(uint32_t)(edi) == 0", "flags from `test edi, edi` in sub_001281C4"),
}

FUNC = re.compile(r"^void ([A-Za-z_][A-Za-z0-9_]*)\(void\)\r?$")
DEAD = re.compile(r"^(?P<ind>\s*)if \(_flags (?P<cmt>/\* (?P<cc>j[a-z]+):[^*]*\*/)\)(?P<rest>.*)$")
TAIL = re.compile(r"g_seh_ebp = ebp; (?P<name>sub_[0-9A-F]{8})\(\); return;")
FUSED = re.compile(r"^\s*if \((?P<mac>TEST_[A-Z]+|CMP_[A-Z]+)\((?P<args>.*)\)\) "
                   r"\{ g_seh_ebp = ebp; (?P<name>sub_[0-9A-F]{8})\(\); return; \}")
STEP = re.compile(r"^\s*(?P<reg>e[abcd]x|e[sd]i|ebp)(?:--|\+\+);\s*$")
REG = r"(?:e[abcd]x|e[sd]i|ebp|esp)"
OPERAND = re.compile(r"^(?:\((?P<cast>uint16_t|uint8_t)\))?\s*\(?(?P<x>%s|(?:LO16|LO8|HI8)\(%s\)"
                     r"|0x[0-9A-Fa-f]+|\d+)\)?$" % (REG, REG))
AND_SELF = re.compile(r"^(?P<x>%s) & (?P=x)$" % REG)
WIDTH = {None: ("uint32_t", "int32_t"), "uint16_t": ("uint16_t", "int16_t"),
         "uint8_t": ("uint8_t", "int8_t")}


def split_args(s):
    """Top-level comma split of a macro argument list."""
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip()); cur = ""
        else:
            cur += ch
    out.append(cur.strip())
    return out


def operand(s):
    """'ebp' / '(uint16_t)eax' / '0' -> (expr, cast) or None if not a register/immediate."""
    m = AND_SELF.match(s)
    if m:                       # `test x, x` lifted as cmp (x & x), 0: same flags
        return m.group("x"), None
    m = OPERAND.match(s)
    return (m.group("x"), m.group("cast")) if m else None


def condition(op, a, b, cast, cc):
    """C expression of the jcc after `op a, b` (op: test|cmp), operands of width cast."""
    u, s = WIDTH[cast]
    if op == "test":
        v = "(%s)(%s)" % (u, a) if a == b else "(%s)((%s) & (%s))" % (u, a, b)
        sv = "(%s)(%s)" % (s, a) if a == b else "(%s)((%s) & (%s))" % (s, a, b)
        return {"je": v + " == 0", "jne": v + " != 0", "js": sv + " < 0", "jns": sv + " >= 0",
                "jl": sv + " < 0", "jge": sv + " >= 0", "jle": sv + " <= 0", "jg": sv + " > 0",
                "jbe": v + " == 0", "ja": v + " != 0", "jb": "0", "jae": "1"}.get(cc)
    ua, ub, sa, sb = ("(%s)(%s)" % (t, x) for t, x in ((u, a), (u, b), (s, a), (s, b)))
    return {"je": "%s == %s" % (ua, ub), "jne": "%s != %s" % (ua, ub),
            "jb": "%s < %s" % (ua, ub), "jae": "%s >= %s" % (ua, ub),
            "jbe": "%s <= %s" % (ua, ub), "ja": "%s > %s" % (ua, ub),
            "jl": "%s < %s" % (sa, sb), "jge": "%s >= %s" % (sa, sb),
            "jle": "%s <= %s" % (sa, sb), "jg": "%s > %s" % (sa, sb)}.get(cc)


def producer_shape(lines, i, fstart):
    """Flags carried into the tail call at lines[i]: (op, a, b, cast, form) or a reason string."""
    line = lines[i]
    m = FUSED.match(line)
    if m:
        args = split_args(m.group("args"))
        if len(args) != 2:
            return "form 1 with %d operands" % len(args)
        pa, pb = operand(args[0]), operand(args[1])
        if not pa or not pb:
            return "form 1 operand is not a register or an immediate (%s)" % m.group("args")
        if pa[1] != pb[1] and not (pb[0][0].isdigit()):
            return "form 1 operands of different widths (%s)" % m.group("args")
        op = "test" if m.group("mac").startswith("TEST_") else "cmp"
        if AND_SELF.match(args[0]) and args[1] == "0":
            op, pb = "test", pa      # `test x, x` lifted as CMP_xx(x & x, 0)
        return (op, pa[0], pb[0], pa[1], 1)
    if line.lstrip().startswith("g_seh_ebp = ebp;"):
        j = i - 1
        while j > fstart and not lines[j].strip():
            j -= 1
        m = STEP.match(lines[j])
        if m:
            return ("test", m.group("reg"), m.group("reg"), None, 2)
        return "no flag-setting instruction just before the tail call"
    return "tail call in an unrecognised statement"


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        sys.exit(__doc__)
    gen, dry = args[0], "--dry-run" in sys.argv
    files = {}
    for path in sorted(glob.glob(os.path.join(gen, "recomp_*.c"))):
        files[path] = io.open(path, encoding="utf-8", errors="surrogateescape", newline="").read().split("\n")

    funcs = {}                      # name -> (path, start, end)
    tails = {}                      # callee -> [(path, line, caller start)]
    callrefs = {}                   # callee -> count of `name();` anywhere
    for path, lines in files.items():
        name = start = None
        for i, l in enumerate(lines):
            m = FUNC.match(l)
            if m:
                name, start = m.group(1), i
            elif l.rstrip("\r") == "}" and name:
                funcs[name] = (path, start, i)
                name = None
            for c in re.findall(r"\b(sub_[0-9A-F]{8})\(\);", l):
                callrefs[c] = callrefs.get(c, 0) + 1
            t = TAIL.search(l)
            if t and name:
                tails.setdefault(t.group("name"), []).append((path, i, start))

    plan, report, patched = [], [], set()
    for name, (path, s, e) in sorted(funcs.items()):
        lines = files[path]
        body = lines[s:e]
        if any(TAG in l for l in body):
            patched.add(name)
            continue
        if not any(l.strip() == "int _flags = 0; /* fallback flag var */" for l in body):
            continue
        dead = [(k + s, DEAD.match(l)) for k, l in enumerate(body) if DEAD.match(l)]
        if not dead:
            continue
        why = None
        # the jcc must be the first instruction: only the label and RECOMP_LOC before it
        first = [k + s for k, l in enumerate(body) if l.strip().startswith("RECOMP_LOC(")]
        if len(dead) != 1 or not first or dead[0][0] != first[0] + 1:
            why = "the dead jcc is not the first instruction"
        prods = tails.get(name, [])
        if not why and (not prods or callrefs.get(name, 0) != len(prods)):
            why = "callers: %d tail call(s) of %d reference(s)" % (len(prods), callrefs.get(name, 0))
        shapes = []
        if not why:
            for ppath, pi, pstart in prods:
                sh = producer_shape(files[ppath], pi, pstart)
                if isinstance(sh, str):
                    why = sh; break
                shapes.append(sh)
        if not why and len(set(sh[:4] for sh in shapes)) != 1:
            why = "producers disagree (%s)" % shapes
        cc = dead[0][1].group("cc")
        cond = None
        if not why:
            op, a, b, cast, form = shapes[0]
            if form == 2 and cc not in ("je", "jne"):
                why = "dec/inc flags with %s (needs CF/OF)" % cc
            else:
                cond = condition(op, a, b, cast, cc)
                if cond is None:
                    why = "unsupported condition %s" % cc
        if why:
            if name in SITES:
                sys.exit("fix_deadflags: %s changed shape (%s): stop rather than guess" % (name, why))
            continue
        if name not in SITES:
            report.append("%s: eligible (form %d, %s -> %s), not listed" % (name, shapes[0][4], cc, cond))
            continue
        bit, want, note = SITES[name]
        if cond != want:
            sys.exit("fix_deadflags: %s: derived `%s`, expected `%s`: stop" % (name, cond, want))
        plan.append((name, path, dead[0][0], dead[0][1], bit, cond, note, len(prods)))

    missing = set(SITES) - set(p[0] for p in plan) - patched
    if missing:
        sys.exit("fix_deadflags: listed site(s) not found as dead branches: %s" % ", ".join(sorted(missing)))
    if not plan:
        print("fix_deadflags: already patched (%d sites)" % len(patched))
        return

    touched = set()
    for name, path, ln, m, bit, cond, note, nprod in plan:
        lines = files[path]
        cr = "\r" if lines[ln].endswith("\r") else ""
        rest = m.group("rest").rstrip("\r")
        lines[ln] = ("%sif ((g_fix_deadflags & 0x%X) ? (%s) : _flags %s)%s %s %s%s"
                     % (m.group("ind"), bit, cond, m.group("cmt"), rest, TAG,
                        "/* " + note + " */", cr))
        touched.add(path)
        print("%s: %s %s restored (%s; %d producer(s), bit 0x%X)"
              % (os.path.basename(path), name, m.group("cc"), cond, nprod, bit))
    for path in touched:
        lines = files[path]
        first = next(i for i, l in enumerate(lines) if FUNC.match(l))
        cr = "\r" if lines[first].endswith("\r") else ""
        lines.insert(first, DECL + cr)
        if not dry:
            io.open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write("\n".join(lines))
    for r in report:
        print("    " + r)
    print("fix_deadflags: %d site(s)%s" % (len(plan), " (dry run)" if dry else ""))


if __name__ == "__main__":
    main()
