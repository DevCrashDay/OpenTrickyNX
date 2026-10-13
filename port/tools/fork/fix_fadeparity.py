#!/usr/bin/env python3
"""Fork pass: the 1 s volume fades of the audio mixer last 1 s again.

sub_0010E210 starts a fade of the mixer's group volumes (step = (target - current)
/ duration / 60); the audio update sub_00115EE0 calls sub_0010E280 once per render
to add the step and stop at the target. Its end test (0x10E2AB..0x10E2D4):

    fcomp [0x1A9F34] ; fnstsw ax      compare 1: step vs 0.0 (fade up or down)
    fcomp [m+0x3B44]                  compare 2: new value vs target
    test ah,5 ; fnstsw ax ; jp ...    PF from compare 1, then AH = compare 2

FPU_PARITY() reads the live compare (g_fpu_cmp), compare 2 by then: both ways, the
fade ends on its first step and jumps to its target in one frame. The
first jp now reads the AH snapshot the lifter takes at the `test` (_fsa). It is the
only parity branch of the XBE fed by a compare replaced before the jump (1,174
scanned).

Once fades take time, extra renders above 60 fps would step them faster: the call
from sub_00115EE0 is skipped in an extra render (fps_cap_skip_fade(), as for the
crowd, fix_crowdemit.py). Both behind XBOX_FIX_FADE (main.c, default 1).

    fix_fadeparity.py GEN_DIR [--dry-run]
Idempotent. Stops if the code around either site changed shape.
"""
import glob, io, os, sys

TAG = "/* fork_fadeparity */"
DECL = "extern int g_fix_fade; extern int fps_cap_skip_fade(void); " + TAG + "\n"
SITES = [  # (function, old line, new line, shape of the 3 lines before)
    ("void sub_0010E280(void)\n",
     "    if (FPU_PARITY(0x05)) goto loc_0010E2D1;",
     "    if (g_fix_fade ? !__builtin_parity(_fsa & 0x05u) : FPU_PARITY(0x05)) goto loc_0010E2D1; "
     "/* jp 0x10E2C8: PF of `test ah,5` on the first fnstsw (_fsa) */ " + TAG + "\n",
     ["    _fsa = (uint32_t)(HI8(eax)); /* flag operand snapshot */",
      "    (void)0; /* test HI8(eax), 5 - flags set for next jcc */",
      "    g_fpu_cmp = _fpu_cmp; FNSTSW_AX(eax); /* fnstsw ax */"]),
    ("void sub_00115EE0(void)\n",
     "    g_seh_ebp = ebp; PUSH32(esp, 0); sub_0010E280(); /* call 0x0010E280 */",
     "    if (!fps_cap_skip_fade()) { g_seh_ebp = ebp; PUSH32(esp, 0); sub_0010E280(); } "
     "/* call 0x0010E280 */ " + TAG + " /* once per tick: fps_cap.c */\n", None),
]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        sys.exit(__doc__)
    gen, dry = args[0], "--dry-run" in sys.argv
    files = {}
    for path in sorted(glob.glob(os.path.join(gen, "recomp_*.c"))):
        src = io.open(path, encoding="utf-8", errors="surrogateescape", newline="").read()
        for site in SITES:
            if "\n" + site[0].rstrip("\n") in src:
                files.setdefault(path, [src, []])[1].append(site)
    if sorted(s[0] for v in files.values() for s in v[1]) != sorted(s[0] for s in SITES):
        sys.exit("fix_fadeparity: expected each of the 2 functions in exactly 1 file")
    n = 0
    for path, (src, sites) in files.items():
        if TAG in src:
            print("%s: already patched" % os.path.basename(path))
            continue
        lines = src.splitlines(True)
        for head, old, new, shape in sites:
            a = next(i for i, l in enumerate(lines) if l.rstrip("\r\n") == head.rstrip("\n"))
            b = next(j for j in range(a, len(lines)) if lines[j].rstrip("\r\n") == "}")
            hit = [k for k in range(a, b) if lines[k].startswith(old)]
            if len(hit) != 1 or (shape and [l.rstrip("\r\n") for l in lines[hit[0] - 3:hit[0]]] != shape):
                sys.exit("fix_fadeparity: %s changed shape (%d sites): stop rather than guess"
                         % (head.strip(), len(hit)))
            nl = lines[hit[0]][len(lines[hit[0]].rstrip("\r\n")):]
            lines[hit[0]] = new.replace("\n", nl)
            print("%s: %s patched" % (os.path.basename(path), head.split()[1].split("(")[0]))
            n += 1
        first = next(i for i, l in enumerate(lines) if l.startswith("void sub_"))
        lines.insert(first, DECL.replace("\n", lines[first][len(lines[first].rstrip("\r\n")):]))
        if not dry:
            io.open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write("".join(lines))
    print("fix_fadeparity: %d site(s)%s" % (n, " (dry run)" if dry else ""))


if __name__ == "__main__":
    main()
