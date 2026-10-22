#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Keep a rebuild from recompiling the whole translated game.

fork_regen.sh rebuilds port/src/recomp/gen/ from zero, so every file in it is
new to make/ninja and all of them compile again, even when the output is the
same byte for byte (the translation is deterministic). The build scripts use
this around it:

  gen_cache.py stamp          print a hash of everything the translation reads
                              (the XBE, the seeds, the tools, port/src outside
                              gen/); unchanged stamp + gen/ present = skip it
  gen_cache.py keep GEN PREV  give each file in GEN that is identical to its
                              copy in PREV (the previous gen/, which
                              fork_regen.sh leaves in build/gen.prev) that
                              copy's time, so only changed files compile
"""
import hashlib
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
INPUTS = ["game_files/default.xbe", "port/src", "port/tools", "xboxrecomp/tools"]
SKIP_DIRS = {"__pycache__", ".git"}
GEN = os.path.join(ROOT, "port", "src", "recomp", "gen")


def stamp():
    h = hashlib.sha256()
    for top in INPUTS:
        p = os.path.join(ROOT, top)
        files = [p] if os.path.isfile(p) else []
        for d, dirs, names in os.walk(p):
            dirs[:] = sorted(x for x in dirs if x not in SKIP_DIRS and os.path.join(d, x) != GEN)
            files += [os.path.join(d, n) for n in sorted(names) if not n.endswith(".pyc")]
        for f in files:
            h.update(os.path.relpath(f, ROOT).replace(os.sep, "/").encode() + b"\0")
            with open(f, "rb") as fh:
                h.update(hashlib.sha256(fh.read()).digest())
    print(h.hexdigest())


def keep(gen, prev):
    same = changed = 0
    for d, _, names in os.walk(gen):
        for n in names:
            f = os.path.join(d, n)
            old = os.path.join(prev, os.path.relpath(f, gen))
            try:
                with open(f, "rb") as a, open(old, "rb") as b:
                    if a.read() != b.read():
                        changed += 1
                        continue
                st = os.stat(old)
                os.utime(f, ns=(st.st_atime_ns, st.st_mtime_ns))
                same += 1
            except OSError:
                changed += 1
    print("   %d translated file(s) changed, %d unchanged (not recompiled)" % (changed, same))


if __name__ == "__main__":
    if sys.argv[1:2] == ["stamp"]:
        stamp()
    elif sys.argv[1:2] == ["keep"] and len(sys.argv) == 4:
        keep(sys.argv[2], sys.argv[3])
    else:
        sys.exit(__doc__)
