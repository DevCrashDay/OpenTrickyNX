#!/usr/bin/env python3
"""Fork pass: no ambient-emitter update in the extra renders above 60 fps.

The race render sub_000AB610 (chunk sub_000AB656) calls sub_00120CF0, the update of
the ambient sound emitters (crowd chants and cheers, track ambience: 40 slots of 0x40
bytes from [0x1F8908]). For each active emitter it either updates it, when the 60 Hz
tick refreshed it since the last call ([e+0x34], set by sub_0011FFD0), or stops its
sound and forgets its bank ([e+0x30] = [e+0x3C] = -1); then it clears [e+0x34].

On the console the render runs once per tick. Above 60 fps the port adds renders
without a tick (port/src/fps_cap.c, extra_render): each one stops every emitter, the
next tick starts it again with a new random bank, which evicts the previous one from
the shared chant slot (sub_0010FC50): the banks are reloaded every frame and no crowd
or ambience is ever heard (end-of-race cutscene, finish area).

The call is guarded by fps_cap_skip_emitters() (true only in an extra render and with
XBOX_FIX_CROWDEMIT != 0, port/src/main.c). The other caller (sub_000E9810) is untouched.

    fix_crowdemit.py GEN_DIR [--dry-run]
Idempotent. Stops if the code around the site changed shape.
"""
import glob, io, os, sys

TAG = "/* fork_crowdemit */"
DECL = "extern int fps_cap_skip_emitters(void); " + TAG + "\n"
FUNC = "void sub_000AB656(void)\n{\n"
CALL = "    g_seh_ebp = ebp; PUSH32(esp, 0); sub_00120CF0(); /* call 0x00120CF0 */\n"
NEW = ("    if (!fps_cap_skip_emitters()) { g_seh_ebp = ebp; PUSH32(esp, 0); sub_00120CF0(); } "
       "/* call 0x00120CF0 */ " + TAG + " /* not in an extra render: fps_cap.c */\n")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        sys.exit(__doc__)
    gen, dry = args[0], "--dry-run" in sys.argv
    hits, calls = [], 0
    for path in sorted(glob.glob(os.path.join(gen, "recomp_*.c"))):
        src = io.open(path, encoding="utf-8", errors="surrogateescape", newline="").read()
        calls += src.count("sub_00120CF0(); /* call 0x00120CF0 */")
        if "void sub_000AB656(void)" in src:
            hits.append((path, src))
    if len(hits) != 1:
        sys.exit("fix_crowdemit: expected sub_000AB656 in exactly 1 file, found %d" % len(hits))
    path, src = hits[0]
    nl = "\r\n" if "\r\n" in src else "\n"
    func, call, new_call, decl = (s.replace("\n", nl) for s in (FUNC, CALL, NEW, DECL))
    start = src.index(func)
    end = src.index(nl + "}" + nl, start)
    body = src[start:end]
    if TAG in body:
        print("%s: already patched" % os.path.basename(path))
        return
    # Two direct callers in the game: sub_000AB656 (race render) and sub_000E9810.
    if body.count(call) != 1 or calls != 2:
        sys.exit("fix_crowdemit: sub_000AB656 / sub_00120CF0 calls changed shape in %s "
                 "(site %d, calls %d): stop rather than guess" % (path, body.count(call), calls))
    new = src[:start] + body.replace(call, new_call, 1) + src[end:]
    first = new.index(nl + "void sub_")
    new = new[:first + len(nl)] + decl + new[first + len(nl):]
    print("%s: sub_000AB656 -> sub_00120CF0 guarded (extra renders)" % os.path.basename(path))
    if not dry:
        io.open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(new)
    print("fix_crowdemit: 1 site%s" % (" (dry run)" if dry else ""))


if __name__ == "__main__":
    main()
