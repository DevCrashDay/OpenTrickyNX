#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Codegen bench: a small x86 program (guest.c: x87 physics, a hash, a jump
# table, indirect calls) compiled with gcc -m32, lifted by xboxrecomp's
# translator, compiled for AArch64 with the guest registers in thread-local
# memory and in CPU registers (xbox_guest_regs.h), and run under qemu. Both
# must print what the x86 program itself prints.
#
#   port/tools/switch/codegen_bench/run.sh [OUT_DIR]     (default build/codegen_bench)
#   COUNT=1 ...   also count the instructions each one executes (slow)
#
# Needs: gcc with -m32 support, aarch64-linux-gnu-gcc, qemu-i386, qemu-aarch64,
# Python 3 with capstone. With Docker and the devkitpro/devkita64 image the
# lifted code is compiled by devkitA64 with the Switch's flags (-mtp=soft);
# without, by aarch64-linux-gnu-gcc. No game code or data is involved.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
OUT="${1:-$ROOT/build/codegen_bench}"
ITERS="${ITERS:-20}"
mkdir -p "$OUT"
cd "$OUT"

# ── the x86 program, and what it prints ────────────────────────────────────
X86="-m32 -O2 -fno-pic -fno-pie -mfpmath=387 -mno-sse -fomit-frame-pointer -fno-asynchronous-unwind-tables
     -fno-stack-protector -ffreestanding -fno-builtin -fcf-protection=none"
LD32="ld -m elf_i386 -static -nostdlib -z noexecstack -Ttext=0x00011000 -Tdata=0x00200000 -Tbss=0x00300000"
gcc $X86 -c "$HERE/guest.c" -o guest.o
$LD32 -e run -o guest.elf guest.o
for n in "$ITERS" 0; do
    gcc -m32 -DITERS=$n -c "$HERE/start32.S" -o start$n.o
    $LD32 -e _start -o x86_$n guest.o start$n.o
done

# ── lift it, as fork_regen.sh would, with the last pass too ───────────────
python3 "$HERE/lift.py" "$ROOT/xboxrecomp"
mkdir -p pass && cp gen.c pass/recomp_bench.c
python3 -I "$ROOT/port/tools/fork/fix_stack_access.py" pass
cp pass/recomp_bench.c gen.c

# ── compile it both ways ───────────────────────────────────────────────────
# Include paths and the register-mode flags, for a tree at $1.
inc()  { echo "-I$1/port/src/posix/compat -I$1/port/src -I$1/port/src/recomp -I$1/xboxrecomp/include -I$1/xboxrecomp/src"; }
regs() { echo "-DXBOX_GUEST_REGS=1 -DXBOX_STACK_UNCHECKED=1 -include $1/xboxrecomp/src/kernel/xbox_guest_regs.h"; }
WARN="-Wno-int-conversion -Wno-incompatible-pointer-types"
if command -v docker >/dev/null && docker image inspect devkitpro/devkita64 >/dev/null 2>&1; then
    CC=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc
    FLAGS="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -ftls-model=local-exec -D__SWITCH__ -O2 -fno-strict-aliasing -fwrapv"
    docker run --rm -v "$ROOT:/src:ro" -v "$OUT:/w" -w /w devkitpro/devkita64 sh -c "
        $CC $FLAGS $WARN $(inc /src) -I/opt/devkitpro/libnx/include -c gen.c -o gen_tls.o &&
        $CC $FLAGS $WARN $(inc /src) -I/opt/devkitpro/libnx/include $(regs /src) -c gen.c -o gen_reg.o &&
        $CC $FLAGS $(regs /src) -c /src/xboxrecomp/src/kernel/xbox_guest_regs.c -o guest_regs.o"
    echo "lifted code compiled by devkitA64 (Switch flags)"
else
    A="aarch64-linux-gnu-gcc -O2 -fno-strict-aliasing -fwrapv"
    $A $WARN $(inc "$ROOT") -c gen.c -o gen_tls.o
    $A $WARN $(inc "$ROOT") $(regs "$ROOT") -c gen.c -o gen_reg.o
    $A $(regs "$ROOT") -c "$ROOT/xboxrecomp/src/kernel/xbox_guest_regs.c" -o guest_regs.o
    echo "lifted code compiled by aarch64-linux-gnu-gcc"
fi
REGS_HOST="$(regs "$ROOT")"
aarch64-linux-gnu-gcc -c "$HERE/readtp.S" -o readtp.o
aarch64-linux-gnu-gcc -O2 -c "$HERE/rt.c" -o rt_tls.o
aarch64-linux-gnu-gcc -O2 $REGS_HOST -c "$HERE/rt.c" -o rt_reg.o
aarch64-linux-gnu-gcc -static -o bench_tls rt_tls.o gen_tls.o readtp.o -lm
aarch64-linux-gnu-gcc -static -o bench_reg rt_reg.o gen_reg.o guest_regs.o readtp.o -lm

# ── run ────────────────────────────────────────────────────────────────────
want="$(qemu-i386 ./x86_$ITERS)"
fail=0
for b in bench_tls bench_reg; do
    got="$(qemu-aarch64 ./$b "$ITERS")"
    if [ "$got" = "$want" ]; then echo "$b: $got (as x86)"; else echo "$b: $got, x86 printed $want"; fail=1; fi
done
if [ "${COUNT:-0}" = 1 ]; then
    count() { local q="$1"; shift; "$q" -one-insn-per-tb -d nochain,exec -D trace.log "$@" >/dev/null
              grep -c '^Trace' trace.log; rm -f trace.log; }
    x=$(( $(count qemu-i386 ./x86_$ITERS) - $(count qemu-i386 ./x86_0) ))
    echo "x86: $x instructions for $ITERS iterations"
    for b in bench_tls bench_reg; do
        a=$(( $(count qemu-aarch64 ./$b "$ITERS") - $(count qemu-aarch64 ./$b 0) ))
        echo "$b: $a instructions, $(awk "BEGIN { printf \"%.2f\", $a / $x }") per x86 instruction"
    done
fi
exit $fail
