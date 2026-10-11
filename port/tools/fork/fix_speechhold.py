#!/usr/bin/env python3
"""Fork pass: restore the dead `jge` of sub_0015A79C (cutscene speech held for its cue).

sub_0015A740 (speech queue, called through sub_00158AC0) ends like this in the XBE:

    0x0015A75F  85 ED        test ebp, ebp
    0x0015A761  75 39        jnz  0x0015A79C
      ...
    0x0015A78D  85 ED        test ebp, ebp
    0x0015A78F  75 0B        jnz  0x0015A79C
      ...
    0x0015A795               pop edi / pop esi / mov eax, ebp / pop ebp / pop ebx / ret
    0x0015A79C  7D F7        jge  0x0015A795      ; ebp >= 0: return ebp
    0x0015A79E               pop edi / pop esi / pop ebp / xor eax, eax / pop ebx / ret

The lifter split the function at 0x0015A79C, between the `test` and the `jge`, so
the chunk sub_0015A79C tests a local `_flags = 0` that nothing sets: the branch is
never taken and sub_0015A740 always returns 0. fixsplitflags.py does not see this
site (its producers have the form `if (TEST_NZ(ebp, ebp)) { sub_0015A79C(); return; }`).

Effect in the game: when the end-of-event cutscene asks for the rider's line
(sub_00123780 -> sub_0011C1E0 -> sub_00158A50), the synchronous start fails, so the
line is not held for the animation cue of the close-up: it is played at once by
the per-frame queue (sub_0011C0D0), over the wide shots, and the close-up of the
rider talking is silent (or, with the speech channel busy, no line at all).

Both producers set the flags with `test ebp, ebp` (OF = 0), so `jge` is taken iff
(int32_t)ebp >= 0. With the runtime switch g_fix_speechhold (XBOX_FIX_SPEECHHOLD,
default 1, port/src/main.c) the condition becomes that; 0 = the current port
(branch never taken).

    fix_speechhold.py GEN_DIR [--dry-run]
Idempotent. Stops if the code around the site changed shape.
"""
import glob, io, os, sys

TAG = "/* fork_speechhold */"
DECL = "extern int g_fix_speechhold; " + TAG + "\n"
FUNC = "void sub_0015A79C(void)\n{\n"
DEAD = "    if (_flags /* jge: greater or equal (signed >=) */) { g_seh_ebp = ebp; sub_0015A795(); return; }\n"
LIVE = ("    if (g_fix_speechhold ? (int32_t)ebp >= 0 : _flags /* jge: greater or equal (signed >=) */) "
        "{ g_seh_ebp = ebp; sub_0015A795(); return; } " + TAG +
        " /* flags from `test ebp, ebp` in sub_0015A740 */\n")
PRODUCER = "if (TEST_NZ(ebp, ebp)) { g_seh_ebp = ebp; sub_0015A79C(); return; }"


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        sys.exit(__doc__)
    gen, dry = args[0], "--dry-run" in sys.argv
    hits, callers = [], 0
    for path in sorted(glob.glob(os.path.join(gen, "recomp_*.c"))):
        src = io.open(path, encoding="utf-8", errors="surrogateescape", newline="").read()
        callers += src.count("sub_0015A79C();")
        if "void sub_0015A79C(void)" in src:
            hits.append((path, src))
    if len(hits) != 1:
        sys.exit("fix_speechhold: expected sub_0015A79C in exactly 1 file, found %d" % len(hits))
    path, src = hits[0]
    nl = "\r\n" if "\r\n" in src else "\n"
    func, dead, live, decl = (s.replace("\n", nl) for s in (FUNC, DEAD, LIVE, DECL))
    start = src.index(func)
    end = src.index(nl + "}" + nl, start)
    body = src[start:end]
    if TAG in body:
        print("%s: already patched" % os.path.basename(path))
        return
    # The only callers are the two `test ebp, ebp ; jnz` of sub_0015A740, both in this file.
    if body.count(dead) != 1 or src.count(PRODUCER) != 2 or callers != 2:
        sys.exit("fix_speechhold: sub_0015A79C / sub_0015A740 changed shape in %s "
                 "(dead jge %d, producers %d, callers %d): stop rather than guess"
                 % (path, body.count(dead), src.count(PRODUCER), callers))
    new = src[:start] + body.replace(dead, live, 1) + src[end:]
    first = new.index(nl + "void sub_")
    new = new[:first + len(nl)] + decl + new[first + len(nl):]
    print("%s: sub_0015A79C jge restored ((int32_t)ebp >= 0, 2 producers)" % os.path.basename(path))
    if not dry:
        io.open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(new)
    print("fix_speechhold: 1 site%s" % (" (dry run)" if dry else ""))


if __name__ == "__main__":
    main()
