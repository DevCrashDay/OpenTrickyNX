#!/usr/bin/env python3
"""Fork pass: guest stack accesses skip the address check.

Every MEM* access resolves its address through xbox_resolve_uncached_alias()
(recomp_types.h): a compare and a branch to the uncached/write-combined alias
and null-page paths. An address that starts at esp is on the thread's stack,
which is always ordinary guest RAM (thread stacks are allocated there), so it
can never take those paths. This rewrites `MEM32(esp + 8)` as
`MEM32_STK(esp + 8)` and likewise for the other widths; recomp_types.h defines
the _STK forms as the plain access under XBOX_NULL_IN_PAGE0 and as the checked
one otherwise, so the output stays correct on every platform.

Runs last: the passes before it match the lifter's MEM32(esp ...) text.
Idempotent.

    fix_stack_access.py GEN_DIR [--dry-run]
"""
import io, os, re, sys

# An accessor whose whole address expression starts with esp and has no
# parentheses of its own (`esp + eax * 4 + 0x10`, not `esp + MEM32(...)`).
ACCESS = re.compile(r"\b(MEM8|MEM16|MEM32|MEM64|SMEM8|SMEM16|SMEM32|MEMF|MEMD)\((esp\b[^()]*)\)")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    dry = "--dry-run" in sys.argv
    gen = args[0]
    total = files = 0
    for f in sorted(os.listdir(gen)):
        if not (f.startswith("recomp_") and f.endswith(".c")):
            continue
        p = os.path.join(gen, f)
        s = io.open(p, encoding="utf-8", errors="surrogateescape", newline="").read()
        new, n = ACCESS.subn(lambda m: "%s_STK(%s)" % (m.group(1), m.group(2)), s)
        if not n:
            continue
        total += n
        files += 1
        if not dry:
            io.open(p, "w", encoding="utf-8", errors="surrogateescape", newline="").write(new)
    print("%s %d stack access(es) in %d file(s)" % ("would rewrite" if dry else "rewrote", total, files))


if __name__ == "__main__":
    main()
