"""Lift guest.elf's functions with xboxrecomp's translator into gen.c.

    lift.py XBOXRECOMP_DIR      (in the folder holding guest.elf; needs capstone)

Also writes image.bin, the program's sections at their addresses, which rt.c
loads as guest memory.
"""
import subprocess, sys
sys.path.insert(0, sys.argv[1])          # repo/xboxrecomp
from tools.recomp import config
from tools.recomp.config import Section
from tools.recomp.translator import FunctionTranslator

elf = "guest.elf"
secs = []
for line in subprocess.check_output(["objdump", "-h", elf], text=True).splitlines():
    p = line.split()
    if len(p) >= 6 and p[0].isdigit() and p[1] in (".text", ".rodata", ".data", ".bss"):
        secs.append((p[1], int(p[2], 16), int(p[3], 16), int(p[5], 16)))
img = bytearray(0x00310000)
data = open(elf, "rb").read()
for name, size, va, off in secs:
    if name != ".bss":
        img[va:va + size] = data[off:off + size]
config._install([Section(n, va, sz, va, 0 if n == ".bss" else sz, n == ".text") for n, sz, va, _ in secs],
                0x11000, 0, "harness")
open("image.bin", "wb").write(img)

funcs = {}
for line in subprocess.check_output(["nm", "-S", "-n", elf], text=True).splitlines():
    p = line.split()
    if len(p) == 4 and p[2] in "tT":
        a, s = int(p[0], 16), int(p[1], 16)
        funcs[a] = {"start": p[0], "_addr": a, "end": a + s, "name": f"sub_{a:08X}"}
tr = FunctionTranslator(bytes(img), funcs)
out = ["#define RECOMP_GENERATED_CODE", '#include "recomp_types.h"', "#include <math.h>", "#include <string.h>"]
out += [f"void sub_{a:08X}(void);" for a in funcs]
for a, f in funcs.items():
    c = tr.translate_function(a, f)
    if not c:
        sys.exit(f"failed: {f['name']}")
    out.append(c)
out.append("const unsigned harness_funcs[] = {" + ",".join(f"0x{a:X}u" for a in funcs) + "};")
out.append("void (*const harness_ptrs[])(void) = {" + ",".join(f"sub_{a:08X}" for a in funcs) + "};")
out.append(f"const int harness_nfuncs = {len(funcs)};")
open("gen.c", "w").write("\n".join(out) + "\n")
print("lifted", len(funcs), "functions")
