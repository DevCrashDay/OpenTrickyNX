#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# OpenTricky for the Nintendo Switch -- build the game from your own SSX Tricky
# (USA) Xbox disc image.
#
#   ./build_switch.sh "path/to/SSX Tricky (USA).iso"
#
# Runs on Linux, macOS or Windows (build_switch.bat, through MSYS2 as for the
# Windows build) with Python 3 (capstone, numpy, pillow) and Docker (Docker
# Desktop on Windows and macOS); the Switch toolchain (devkitPro's devkitA64, libnx, SDL2 and Mesa) is
# the devkitpro/devkita64 image, pulled on first use. Set DEVKITPRO instead to
# use an installed devkitPro and skip Docker.
#
# VULKAN=1 builds the Vulkan renderer (NVK, as flatout2-sw / fo2x) instead of
# OpenGL, in port/build-switch-vk/. It needs two install prefixes:
#   NVK_SDK      mesa-switch built with NVK (include/vulkan, lib/libvulkan.a): the
#                portlibs/switch folder of the "mesa-switch-nvk-sdk" download
#                from this repository's "Mesa NVK SDK" GitHub Actions run
#   GLSLANG_DIR  glslang built for the Switch (port/tools/build_glslang_switch.sh;
#                built into build/glslang-switch on first use when not set)
#
# SWITCH_JOBS sets how many files compile at once (default: every core). The
# translated files take a few GB of memory each; lower it when the build
# stalls with Docker short of memory, e.g. SWITCH_JOBS=2.
#
# SWITCH_OPTS passes CMake options to the build folder, which keeps them for
# later builds, e.g. SWITCH_OPTS="-DOT_GUEST_REGS=OFF" (docs/switch.md).
#
# Result: dist/switch/opentricky/"SSX Tricky.nro". Copy that folder to
# sdmc:/switch/ with your disc image in it -- see docs/switch.md. The build
# contains code translated from your copy of the game: keep it to yourself.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

ISO="${1:-}"
if [ -z "$ISO" ] && [ ! -f game_files/default.xbe ]; then
    echo "usage: ./build_switch.sh \"path/to/SSX Tricky (USA).iso\""
    exit 2
fi
# On Windows (MSYS2): Docker wants a Windows path for the mounted folder, and
# MSYS2 must leave the container's own paths (/src, /opt/...) alone.
ON_WINDOWS=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) ON_WINDOWS=1 ;; esac

echo "== [0/4] checking the tools"
PY="${PYTHON:-}"
if [ -z "$PY" ] && [ "$ON_WINDOWS" = 1 ]; then
    # MSYS2's own Python, never the Windows "python3" placeholder (it opens
    # the Microsoft Store and waits).
    PY="${MINGW_PREFIX:-/ucrt64}/bin/python.exe"
    if [ ! -x "$PY" ]; then
        echo "MSYS2's Python is not installed. In the MSYS2 UCRT64 shell:"
        echo "  pacman -S --needed mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-capstone mingw-w64-ucrt-x86_64-python-numpy mingw-w64-ucrt-x86_64-python-pillow mingw-w64-ucrt-x86_64-binutils"
        exit 1
    fi
elif [ -z "$PY" ]; then
    PY=python3
    command -v python3 >/dev/null 2>&1 || PY=python
fi
"$PY" -c "import capstone, numpy, PIL" </dev/null 2>/dev/null || {
    if [ "$ON_WINDOWS" = 1 ]; then
        echo "Python needs capstone, numpy and pillow. In the MSYS2 UCRT64 shell:"
        echo "  pacman -S --needed mingw-w64-ucrt-x86_64-python-capstone mingw-w64-ucrt-x86_64-python-numpy mingw-w64-ucrt-x86_64-python-pillow"
    else
        echo "Python needs capstone, numpy and pillow:  $PY -m pip install capstone numpy pillow"
    fi
    exit 1
}
# The fix-up passes disassemble with GNU objdump.
command -v objdump >/dev/null 2>&1 || {
    if [ "$ON_WINDOWS" = 1 ]; then
        echo "objdump is missing. In the MSYS2 UCRT64 shell:  pacman -S --needed mingw-w64-ucrt-x86_64-binutils"
    else
        echo "objdump is missing: install binutils."
    fi
    exit 1
}
# devkitPro's own Windows installer sets DEVKITPRO without the Switch CMake
# wrapper: build natively only when that is really there, otherwise in Docker.
SWITCH_CMAKE="${DEVKITPRO:-/nonexistent}/portlibs/switch/bin/aarch64-none-elf-cmake"
NATIVE=0
[ -n "${DEVKITPRO:-}" ] && [ -f "$SWITCH_CMAKE" ] && NATIVE=1
if [ "$NATIVE" = 0 ] && ! command -v docker >/dev/null 2>&1; then
    echo "Needs Docker (for the devkitpro/devkita64 image) or an installed devkitPro (DEVKITPRO=...)."
    exit 1
fi

VULKAN="${VULKAN:-0}"
if [ "$VULKAN" = 1 ]; then
    # C:\... from a Command Prompt (build_switch.bat): as an MSYS2 path
    if [ "$ON_WINDOWS" = 1 ]; then
        [ -z "${NVK_SDK:-}" ] || NVK_SDK="$(cygpath -u "$NVK_SDK")"
        [ -z "${GLSLANG_DIR:-}" ] || GLSLANG_DIR="$(cygpath -u "$GLSLANG_DIR")"
    fi
    [ -n "${NVK_SDK:-}" ] && [ -d "$NVK_SDK" ] || { echo "VULKAN=1 needs NVK_SDK (an install prefix), see the top of this script."; exit 1; }
    if [ -z "${GLSLANG_DIR:-}" ]; then
        GLSLANG_DIR="$ROOT/build/glslang-switch"
        [ -f "$GLSLANG_DIR/lib/libglslang.a" ] || bash port/tools/build_glslang_switch.sh build/glslang-switch
    fi
    [ -f "$NVK_SDK/lib/libvulkan.a" ] || [ -f "$NVK_SDK/lib/libvulkan_nouveau.a" ] ||
        { echo "No lib/libvulkan.a in NVK_SDK=$NVK_SDK"; exit 1; }
    [ -f "$GLSLANG_DIR/lib/libglslang.a" ] || { echo "No lib/libglslang.a in GLSLANG_DIR=$GLSLANG_DIR (port/tools/build_glslang_switch.sh builds it)"; exit 1; }
fi

if [ -n "$ISO" ]; then
    echo "== [1/4] reading your disc image"
    "$PY" port/tools/extract_iso.py "$ISO" game_files
else
    echo "== [1/4] game_files/default.xbe already there"
fi

echo "== [2/4] translating the game (about 3 minutes, the first time)"
mkdir -p build
GEN=port/src/recomp/gen
STAMP=build/gen.stamp
NEW_STAMP="$("$PY" port/tools/gen_cache.py stamp | tr -d '\r')"
if [ -d "$GEN" ] && [ "$(cat "$STAMP" 2>/dev/null)" = "$NEW_STAMP" ]; then
    echo "   nothing it reads has changed since the last build: skipped"
else
    rm -f "$STAMP"
    PYTHON="$PY" bash port/tools/fork_regen.sh
    # gen/ was rebuilt from zero: files that came out the same keep their old
    # time, so the compile step only redoes the ones that changed.
    [ ! -d build/gen.prev ] || "$PY" port/tools/gen_cache.py keep "$GEN" build/gen.prev
    echo "$NEW_STAMP" > "$STAMP"
fi

echo "== [3/4] compiling for the Switch (10 to 20 minutes the first time, then only what changed)"
BUILD=port/build-switch
VK_ARGS=""
DOCKER_VK=()
if [ "$VULKAN" = 1 ]; then
    BUILD=port/build-switch-vk
    if [ "$NATIVE" = 1 ]; then
        VK_ARGS="-DXBOX_VULKAN=ON -DNVK_SDK=$NVK_SDK -DGLSLANG_DIR=$GLSLANG_DIR"
    else
        # the two prefixes, mounted read-only into the container
        NVK_M="$NVK_SDK"; GL_M="$GLSLANG_DIR"
        [ "$ON_WINDOWS" = 1 ] && { NVK_M="$(cygpath -m "$NVK_SDK")"; GL_M="$(cygpath -m "$GLSLANG_DIR")"; }
        DOCKER_VK=(-v "$NVK_M:/opt/nvk:ro" -v "$GL_M:/opt/glslang:ro")
        VK_ARGS="-DXBOX_VULKAN=ON -DNVK_SDK=/opt/nvk -DGLSLANG_DIR=/opt/glslang"
    fi
fi
# Only the Release flags are set here: CMAKE_C_FLAGS would replace the Switch
# toolchain's own (-D__SWITCH__, the CPU, -fPIE).
CONFIGURE="cmake -S port -B $BUILD -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS_RELEASE='-O2 -DNDEBUG -ffile-prefix-map=/src/=' $VK_ARGS"
# A folder configured by an earlier version of this script, with the Switch
# toolchain's flags replaced: start it again.
if grep -q "^CMAKE_C_FLAGS:STRING=-ffile-prefix-map" "$BUILD/CMakeCache.txt" 2>/dev/null; then
    rm -rf "$BUILD"
fi
# The Docker build needs a folder configured in Docker (at /src): CMake
# refuses one configured anywhere else, so start it again.
if [ "$NATIVE" = 0 ] && [ -f "$BUILD/CMakeCache.txt" ] &&
   ! grep -qxF "CMAKE_CACHEFILE_DIR:INTERNAL=/src/$BUILD" "$BUILD/CMakeCache.txt"; then
    echo "   $BUILD was configured elsewhere: configuring it again"
    rm -rf "$BUILD"
fi
if [ "$NATIVE" = 1 ]; then
    CMAKE="$SWITCH_CMAKE"
    [ -f "$BUILD/CMakeCache.txt" ] || eval "${CONFIGURE/cmake/$CMAKE}"
    [ -z "${SWITCH_OPTS:-}" ] || cmake $SWITCH_OPTS "$BUILD"
    cmake --build "$BUILD" -j "${SWITCH_JOBS:-$(nproc 2>/dev/null || echo 4)}"
else
    if [ "$ON_WINDOWS" = 1 ]; then
        MOUNT="$(cygpath -m "$ROOT")"; USER_ARGS=()
        export MSYS2_ARG_CONV_EXCL='*'
    else
        MOUNT="$ROOT"; USER_ARGS=(-u "$(id -u):$(id -g)")
    fi
    docker info >/dev/null 2>&1 || { echo "Docker is not running: start Docker (Docker Desktop on Windows) and try again."; exit 1; }
    docker run --rm "${USER_ARGS[@]}" ${DOCKER_VK[@]+"${DOCKER_VK[@]}"} -v "$MOUNT:/src" -w /src devkitpro/devkita64 bash -c "
        set -e
        [ -f $BUILD/CMakeCache.txt ] || ${CONFIGURE/cmake//opt/devkitpro/portlibs/switch/bin/aarch64-none-elf-cmake}
        ${SWITCH_OPTS:+cmake $SWITCH_OPTS $BUILD}
        cmake --build $BUILD -j ${SWITCH_JOBS:-\$(nproc)}"
fi

echo "== [4/4] dist/switch/opentricky"
OUT=dist/switch/opentricky
mkdir -p "$OUT"
cp "$BUILD/SSX Tricky.nro" "$OUT/"
# function names for XBOX_PROFILE's report
[ ! -f "$BUILD/SSX Tricky.sym" ] || cp "$BUILD/SSX Tricky.sym" "$OUT/"
# The settings file, unless one is there already (it may hold your changes).
[ -f "$OUT/SSX Tricky env.txt" ] || cp "port/switch/SSX Tricky env.txt" "$OUT/"
echo
echo "Done: $OUT/SSX Tricky.nro (settings: $OUT/SSX Tricky env.txt)"
echo "Copy $OUT to sdmc:/switch/ on your SD card, put your disc image (.iso) in it,"
echo "and start it from the Homebrew Menu opened over a game (hold R), see docs/switch.md."
